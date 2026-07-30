#include "task-host.hpp"

#include "capability-surface.hpp"
#include "framework-bundle.hpp"
#include "operator-session.hpp"
#include "script-validator.hpp"
#include "task-context.hpp"
#include "task-loader.hpp"

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/ids.hpp>

#include <engine/runtime-loader.hpp>
#include <engine/session.hpp>

#include <script/engine.hpp>

#include <trace/event.hpp>
#include <trace/file-sink.hpp>
#include <trace/recorder.hpp>

#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <random>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace uf::task
{
    namespace
    {
        // What the host knows about one run before its VM exists: which project
        // and task it addresses, the bytes the task was compiled from, and the
        // seed its RNG will draw from.
        //
        // The framework version and bundle hash, and the Luau compiler version,
        // are deliberately NOT part of it. They are properties of this binary
        // rather than of the run, and runStartedEvent reads them itself, so no
        // caller can ship a run.started that two different framework builds would
        // write identically -- which is the whole point of stamping them.
        struct RunStartSpec final
        {
            std::string projectId{};
            std::string taskName{};
            std::string sourceHash{};
            uint64      seed{};
        };

        [[nodiscard]]
        auto runStartedEvent(RunStartSpec const& spec) -> trace::TraceEvent
        {
            return trace::TraceEvent{
                .kind = trace::TraceEventKind::RunStarted,
                .run  = trace::TraceEvent::Run{
                    .projectId        = spec.projectId,
                    .taskName         = spec.taskName,
                    .sourceHash       = spec.sourceHash,
                    .frameworkVersion = std::string{frameworkVersion()},
                    .frameworkHash    = std::string{frameworkBundleHash()},
                    .luauVersion      = luauRuntimeVersion(),
                    .seed             = spec.seed,
                },
            };
        }

        [[nodiscard]]
        auto runResourcesValidatedEvent(
            ScriptResourceReport const& report
        ) -> trace::TraceEvent
        {
            return trace::TraceEvent{
                .kind      = trace::TraceEventKind::RunResourcesValidated,
                .resources = trace::TraceEvent::Resources{
                    .recognizers = report.recognizers,
                    .pages       = report.pages,
                },
            };
        }

        // The closing line of the run bracket, written from the same report the
        // caller receives. Both read one classification -- TaskRunReport::outcome
        // -- so the wire outcome and the reported outcome cannot drift apart. An
        // error carrying no automation kind is still named, as InternalInvariant,
        // so the line always identifies a kind.
        [[nodiscard]]
        auto runFinishedEvent(TaskRunReport const& report) -> trace::TraceEvent
        {
            auto wireOutcome = trace::RunOutcome::Completed;
            switch (report.outcome())
            {
            case TaskRunOutcome::Completed:
                wireOutcome = trace::RunOutcome::Completed;
                break;
            case TaskRunOutcome::Cancelled:
                wireOutcome = trace::RunOutcome::Cancelled;
                break;
            case TaskRunOutcome::Failed:
                wireOutcome = trace::RunOutcome::Failed;
                break;
            }

            auto errorKind = std::optional<AutomationErrorKind>{};
            if (report.failure)
            {
                errorKind = automationErrorKind(*report.failure)
                    .value_or(AutomationErrorKind::InternalInvariant);
            }

            return trace::TraceEvent{
                .kind       = trace::TraceEventKind::RunFinished,
                .runOutcome = wireOutcome,
                .errorKind  = errorKind,
            };
        }

        // A fresh seed for one run's deterministic RNG.
        //
        // It comes from std::random_device -- the host's non-deterministic
        // entropy source -- and is drawn once per run, never from a constant and
        // never from the clock. It is recorded in run.started because it is the
        // only replay input the host controls: the sandbox removes math.random
        // and the script can read no clock at all, so this seed plus the same
        // observation sequence reproduces a run exactly. A seed that was silently
        // the same on every run would still look correct in a trace while
        // destroying that property, which is why the fixed placeholder default
        // this replaced is gone.
        [[nodiscard]]
        auto drawRunSeed() -> uint64
        {
            auto device = std::random_device{};
            auto const high = static_cast<uint64>(device());
            auto const low  = static_cast<uint64>(device());
            return (high << 32U) | low;
        }
    }

    auto TaskRunReport::outcome() const noexcept -> TaskRunOutcome
    {
        if (!failure)
        {
            return TaskRunOutcome::Completed;
        }
        if (automationErrorKind(*failure) == AutomationErrorKind::Cancelled)
        {
            return TaskRunOutcome::Cancelled;
        }
        return TaskRunOutcome::Failed;
    }

    // One loaded project instance. It owns everything a run of that project
    // needs and nothing a single run owns: the recognition runtime and the
    // capability surface survive every run, while the trace recorder, the engine
    // session, the task context and the VM are built and destroyed inside one
    // startTask call.
    //
    // Non-movable, and therefore held through a unique_ptr by the host: it
    // contains an std::stop_callback bound to its own stop source, so its
    // address must not change.
    class TaskHost::Generation final
    {
        // Requests the generation's own stop source when the host-supplied
        // external token stops, so cancel() and an outside stop feed one source
        // and neither can shadow the other.
        //
        // It observes an std::stop_source owned by the same Generation. That
        // source is declared before the callback holding the pointer; members are
        // destroyed in reverse declaration order and ~stop_callback blocks until
        // an in-flight invocation returns, so the observed source can never die
        // under the callback.
        struct ExternalStopBridge final
        {
            std::stop_source* p_target{nullptr};

            auto operator()() const noexcept -> void
            {
                if (p_target != nullptr)
                {
                    p_target->request_stop();
                }
            }
        };

        GenerationId          m_id;
        std::filesystem::path m_projectRoot;
        std::string           m_projectId;
        engine::LoadedRuntime m_runtime;
        CapabilitySurface     m_surface;

        std::stop_source                       m_stop{};
        std::stop_callback<ExternalStopBridge> m_externalStop;

        TaskStatus m_status{};

        // The front-end that owns this generation, empty until one drives it. See
        // TaskHost::startOperatorSession for why the latch lives here.
        std::optional<trace::FrontEnd> m_frontEnd{};

    public:
        Generation(
            GenerationId id,
            std::filesystem::path projectRoot,
            std::string projectId,
            engine::LoadedRuntime runtime,
            CapabilitySurface surface,
            std::stop_token externalCancellation
        )
            : m_id{id}
            , m_projectRoot{std::move(projectRoot)}
            , m_projectId{std::move(projectId)}
            , m_runtime{std::move(runtime)}
            , m_surface{std::move(surface)}
            , m_externalStop{
                  std::move(externalCancellation),
                  ExternalStopBridge{&m_stop},
              }
        {
        }

        Generation(Generation const&) = delete;
        Generation(Generation&&) = delete;
        auto operator=(Generation const&) -> Generation& = delete;
        auto operator=(Generation&&) -> Generation& = delete;

        ~Generation() = default;

        [[nodiscard]] auto identity() const noexcept -> GenerationId
        {
            return m_id;
        }

        [[nodiscard]]
        auto projectRoot() const noexcept UF_LIFETIME_BOUND
            -> std::filesystem::path const&
        {
            return m_projectRoot;
        }

        [[nodiscard]]
        auto projectId() const noexcept UF_LIFETIME_BOUND -> std::string const&
        {
            return m_projectId;
        }

        [[nodiscard]]
        auto runtime() const noexcept UF_LIFETIME_BOUND
            -> engine::LoadedRuntime const&
        {
            return m_runtime;
        }

        [[nodiscard]]
        auto surface() const noexcept UF_LIFETIME_BOUND -> CapabilitySurface const&
        {
            return m_surface;
        }

        [[nodiscard]] auto cancellation() const noexcept -> std::stop_token
        {
            return m_stop.get_token();
        }

        auto requestCancel() noexcept -> void
        {
            static_cast<void>(m_stop.request_stop());
        }

        [[nodiscard]] auto status() const -> TaskStatus
        {
            auto snapshot                  = m_status;
            snapshot.cancellationRequested = m_stop.stop_requested();
            return snapshot;
        }

        // Latches `frontEnd` as this generation's owner, or refuses because the other
        // one already owns it.
        //
        // Idempotent under the same front-end and permanent under a different one:
        // the ledger below this generation holds one open cycle, and two policy
        // sources sharing it is the failure this exists to make unrepresentable. It
        // is reported as UnsupportedCapability rather than as an invariant failure
        // because asking is legitimate -- the caller simply cannot have it -- and
        // because nothing in this binary is broken when it happens.
        [[nodiscard]] auto claimFrontEnd(trace::FrontEnd frontEnd) -> Status
        {
            if (!m_frontEnd.has_value())
            {
                m_frontEnd = frontEnd;
                return ok();
            }
            if (*m_frontEnd == frontEnd)
            {
                return ok();
            }
            return fail(
                AutomationErrorKind::UnsupportedCapability,
                std::format(
                    "generation {} is already driven by the {} front-end; a task run "
                    "and an operator session must not share one generation",
                    m_id.value(),
                    *m_frontEnd == trace::FrontEnd::Task ? "task" : "operator"
                )
            );
        }

        auto noteRunStarted(std::string taskName) -> void
        {
            m_status.lastOutcome = std::nullopt;
            m_status.taskName    = std::move(taskName);
        }

        auto noteRunFinished(TaskRunOutcome outcome) noexcept -> void
        {
            m_status.lastOutcome = outcome;
        }
    };

    TaskHost::TaskHost() = default;

    TaskHost::~TaskHost() = default;

    auto TaskHost::findGeneration(GenerationId id) noexcept -> Generation*
    {
        for (auto const& p_generation : m_generations)
        {
            if (p_generation->identity() == id)
            {
                return p_generation.get();
            }
        }
        return nullptr;
    }

    auto TaskHost::loadProject(
        std::filesystem::path const& projectRoot,
        TaskHostConfig const& config
    ) -> Result<GenerationId>
    {
        UF_TRY_VALUE(loaded, engine::loadRuntimeProject(projectRoot));
        UF_TRY_VALUE(
            surface,
            CapabilitySurface::create(loaded.runtime.manifest().catalog())
        );

        auto projectId = std::string{
            loaded.runtime.manifest().catalog().projectId().value()
        };

        auto const id = GenerationId{m_nextGenerationValue};
        ++m_nextGenerationValue;
        m_generations.emplace_back(
            std::make_unique<Generation>(
                id,
                projectRoot,
                std::move(projectId),
                std::move(loaded),
                std::move(surface),
                config.externalCancellation
            )
        );
        return id;
    }

    auto TaskHost::startTask(
        GenerationId generation,
        std::string_view taskName,
        TaskRunConfig config
    ) -> Result<TaskRunReport>
    {
        auto* const p_generation = findGeneration(generation);
        if (p_generation == nullptr)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "no loaded project for generation {}",
                    generation.value()
                )
            );
        }

        // Claim the generation for the task front-end before anything else. It runs
        // ahead of loading the task so a generation an operator already owns refuses
        // here rather than after reading and validating a script it will never run.
        UF_TRY(p_generation->claimFrontEnd(trace::FrontEnd::Task));

        // Source the task from its owning project and validate every uf
        // reference before anything observable exists: a missing or unsafe task
        // name, or a reference the capability surface cannot resolve, must fail
        // before a VM is created (annotation-design 4) and before a trace file
        // is opened, so a misspelled name leaves no evidence behind.
        UF_TRY_VALUE(loadedTask, loadTask(p_generation->projectRoot(), taskName));
        UF_TRY_VALUE(
            resourceReport,
            validateScriptResources(
                loadedTask.source,
                loadedTask.name,
                p_generation->surface()
            )
        );
        p_generation->noteRunStarted(loadedTask.name);

        // The recorder owns this run's single evidence stream, and every layer
        // below borrows it (see the trace lifetime contracts on
        // engine::EngineSession and TaskContext). It is declared before all of
        // them and held through a unique_ptr, so its address is fixed for the
        // whole run and every borrower -- all of them locals of this scope -- is
        // destroyed before it, on the normal path and on every early return.
        UF_TRY_VALUE(traceSink, trace::FileTraceSink::create(config.tracePath));
        auto const runId = TaskRunId{m_nextRunValue};
        ++m_nextRunValue;
        auto recorder = std::make_unique<trace::TraceRecorder>(
            std::move(traceSink),
            runId,
            generation,
            trace::FrontEnd::Task
        );

        auto const seed = drawRunSeed();

        // The run identity opens the stream before the VM exists, so every later
        // event is attributable to this exact task build -- including the
        // framework version and bundle hash, which runStartedEvent reads from
        // this binary rather than taking from here.
        UF_TRY(
            recorder->emit(
                runStartedEvent(
                    RunStartSpec{
                        .projectId  = p_generation->projectId(),
                        .taskName   = loadedTask.name,
                        .sourceHash = loadedTask.hash.hex(),
                        .seed       = seed,
                    }
                )
            )
        );
        UF_TRY(recorder->emit(runResourcesValidatedEvent(resourceReport)));

        // The session takes the runtime by value, so the generation hands it a
        // copy and keeps its own: a generation outlives its runs and must still
        // have a runtime for the next one. The copy is per run, never per frame.
        UF_TRY_VALUE(
            session,
            engine::EngineSession::create(
                p_generation->runtime(),
                std::move(config.frameSource),
                std::move(config.actionSink),
                *recorder,
                engine::EngineSessionConfig{
                    .liveFingerprint         = config.liveFingerprint,
                    .maximumPixelComparisons = config.maximumPixelComparisons,
                    .recognitionTimeout      = config.recognitionTimeout,
                    .maxActionFrameAge       = config.maxActionFrameAge,
                    .cancellation            = p_generation->cancellation(),
                }
            )
        );

        // The context owns the session and borrows the same recorder, and must
        // outlive the VM that binds it, so it is declared before the Engine and
        // destroyed after it. The generation's one stop token drives both the
        // engine (which returns Cancelled) and the VM interrupt (which
        // hard-breaks the task thread), so a cancellation is a single source and
        // never two.
        auto context = TaskContext{
            std::move(session),
            *recorder,
            TaskContextConfig{
                .cancellation = p_generation->cancellation(),
                .randomSeed   = seed,
            },
        };

        auto report = TaskRunReport{
            .taskName   = loadedTask.name,
            .sourceHash = loadedTask.hash.hex(),
            .seed       = seed,
            .tracePath  = config.tracePath,
        };

        // The VM boots two environments. The trusted framework bundle loads
        // under the framework environment and is handed the private capability
        // surface as its chunk argument; the task script below runs under a
        // project environment that is an explicit whitelist and holds no route
        // back to the framework's. So the script reaches the uf data tables
        // and the framework's own `ctx`, and no primitive by any route.
        //
        // A VM that cannot be built at all -- a generation already cancelled, so
        // the interrupt breaks the framework boot, or a framework module that
        // will not load -- ends this RUN, not this call. run.started is already
        // in the trace by now, so the run happened and has to be described; a
        // bare Result failure here would leave a run bracket that never closed.
        auto vm = script::Engine::create(
            script::EngineConfig{
                .cancellation               = p_generation->cancellation(),
                .frameworkModules           = frameworkScriptModules(),
                .installHostTables          = p_generation->surface().installer(),
                .installPrivateCapabilities =
                    CapabilitySurface::privateCapabilities(context),
                .projectGlobals          = CapabilitySurface::projectGlobals(),
                .frameworkProjectGlobals = frameworkProjectGlobals(),
                .classifyRaisedError     = CapabilitySurface::raisedErrorClassifier(),
            }
        );
        if (!vm)
        {
            report.failure = std::move(vm).error();
        }
        else
        {
            // The script's numeric return carries no success meaning of its own,
            // so it is discarded: a Tier B or Tier C failure surfaces as an error
            // here, and a clean return means the task ran to completion.
            auto runResult = vm->runNumber(loadedTask.source, loadedTask.name);
            if (!runResult)
            {
                report.failure = std::move(runResult).error();
            }
        }

        // Two verdicts the script's own return cannot express, folded in before
        // the closing line is built so run.finished reports the run that actually
        // happened.
        //
        // A generation the host spent terminally -- a cancellation, or a framework
        // bug the trace state machine caught -- ended there whatever the script
        // did afterwards. Design section 9's rule 5 latches that in C++ precisely
        // so a project pcall cannot turn it into a completed run, and this is
        // where the latch is read back.
        //
        // A step or an interrupt match still open at run.finished is the framework
        // failing to close what it opened, which section 12 makes a
        // Failed(InternalInvariant) run. Both defer to a failure the run already
        // has: an unclosed step under a cancelled run is the cancel's consequence,
        // not a second cause.
        if (!report.failure)
        {
            auto const terminal = context.terminalKind();
            if (terminal.has_value())
            {
                report.failure = fail(
                    *terminal,
                    "the task generation was spent before the script returned"
                ).error();
            }
        }
        if (!report.failure)
        {
            auto scopes = recorder->requireScopesClosed();
            if (!scopes)
            {
                report.failure = std::move(scopes).error();
            }
        }

        auto finishStatus = recorder->emit(runFinishedEvent(report));
        if (!report.failure && !finishStatus)
        {
            // The run itself succeeded but its closing evidence was lost, which
            // leaves an incomplete trace and so cannot be reported as completed.
            // The run's own failure always takes precedence over the sink's, so
            // this only reaches the report when there was no other failure.
            report.failure = std::move(finishStatus).error();
        }

        p_generation->noteRunFinished(report.outcome());
        return report;
    }

    auto TaskHost::startOperatorSession(
        GenerationId generation,
        TaskRunConfig config
    ) -> Result<std::unique_ptr<OperatorSession>>
    {
        auto* const p_generation = findGeneration(generation);
        if (p_generation == nullptr)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "no loaded project for generation {}",
                    generation.value()
                )
            );
        }

        UF_TRY(p_generation->claimFrontEnd(trace::FrontEnd::Operator));

        auto const runId = TaskRunId{m_nextRunValue};
        ++m_nextRunValue;

        // The session takes the runtime by value for the same reason a task run does:
        // a generation outlives its sessions and must still have a runtime for the
        // next one, so it hands over a copy and keeps its own.
        return OperatorSession::create(
            p_generation->runtime(),
            std::move(config),
            OperatorSession::Spec{
                .projectId    = p_generation->projectId(),
                .surface      = p_generation->surface(),
                .cancellation = p_generation->cancellation(),
            },
            runId,
            generation
        );
    }

    auto TaskHost::cancel(GenerationId generation) -> Status
    {
        auto* const p_generation = findGeneration(generation);
        if (p_generation == nullptr)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "no loaded project for generation {}",
                    generation.value()
                )
            );
        }
        p_generation->requestCancel();
        return ok();
    }

    auto TaskHost::queryTask(GenerationId generation) -> Result<TaskStatus>
    {
        auto* const p_generation = findGeneration(generation);
        if (p_generation == nullptr)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "no loaded project for generation {}",
                    generation.value()
                )
            );
        }
        return p_generation->status();
    }

    auto TaskHost::pause(GenerationId) -> Status
    {
        return fail(
            AutomationErrorKind::UnsupportedCapability,
            "pausing a task is a P2 capability this host does not implement"
        );
    }

    auto TaskHost::resume(GenerationId) -> Status
    {
        return fail(
            AutomationErrorKind::UnsupportedCapability,
            "resuming a task is a P2 capability this host does not implement"
        );
    }

    auto TaskHost::subscribeEvents(ITaskEventSink&) -> Status
    {
        return fail(
            AutomationErrorKind::UnsupportedCapability,
            "task event subscription is a P2 capability this host does not "
            "implement"
        );
    }
}
