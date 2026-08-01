#include "exploration-session.hpp"

#include <task/framework-bundle.hpp>
#include <task/script-bindings.hpp>
#include <task/task-context.hpp>
#include <task/task-host.hpp>
#include <task/task-loader.hpp>

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/ids.hpp>

#include <engine/session.hpp>

#include <script/engine.hpp>

#include <trace/event.hpp>
#include <trace/file-sink.hpp>
#include <trace/recorder.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace uf::task
{
    namespace
    {
        // The opening line of an exploration session's run bracket.
        //
        // WHICH MEMBERS IT FILLS, AND WHY THEY DIFFER FROM AN OPERATOR'S. A
        // trusted Luau framework DOES run here, so the framework version, the
        // bundle hash and the Luau compiler version are named: an agent's chunk
        // calls into `explore`, `observe` and `model`, and a session that did not
        // say which build of those it ran against would not be attributable. The
        // seed is real for the same reason -- the exploration surface carries
        // `random`.
        //
        // The two that stay empty are the task name and the source hash, and both
        // absences are accurate. There is no ONE source: a session is a sequence
        // of chunks the agent wrote as it went, each hashed nowhere and none of
        // them addressable as (project, task). What names a chunk is the id its
        // queue line carried, which reaches the trace through the chunk name in a
        // raised error rather than through this line.
        [[nodiscard]]
        auto explorationRunStartedEvent(
            std::string const& projectId,
            uint64 seed
        ) -> trace::TraceEvent
        {
            return trace::TraceEvent{
                .kind = trace::TraceEventKind::RunStarted,
                .run  = trace::TraceEvent::Run{
                    .projectId        = projectId,
                    .frameworkVersion = std::string{frameworkVersion()},
                    .frameworkHash    = std::string{frameworkBundleHash()},
                    .luauVersion      = luauRuntimeVersion(),
                    .seed             = seed,
                },
            };
        }

        [[nodiscard]]
        auto explorationRunFinishedEvent(
            TaskRunReport const& report
        ) -> trace::TraceEvent
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
    }

    ExplorationSession::ExplorationSession(
        CreateTag,
        std::unique_ptr<trace::TraceRecorder> recorder,
        engine::EngineSession session,
        TaskContextConfig contextConfig,
        std::filesystem::path tracePath,
        uint64 seed
    ) noexcept
        : m_recorder{std::move(recorder)}
        , m_context{std::move(session), *m_recorder, std::move(contextConfig)}
        , m_tracePath{std::move(tracePath)}
        , m_seed{seed}
    {
    }

    auto ExplorationSession::create(
        TaskRunConfig config,
        Spec spec,
        TaskRunId runId,
        GenerationId generationId
    ) -> Result<std::unique_ptr<ExplorationSession>>
    {
        UF_TRY_VALUE(traceSink, trace::FileTraceSink::create(config.tracePath));
        auto recorder = std::make_unique<trace::TraceRecorder>(
            std::move(traceSink),
            runId,
            generationId,
            trace::FrontEnd::Annotation
        );

        UF_TRY(
            recorder->emit(explorationRunStartedEvent(spec.projectId, spec.seed))
        );

        // No run.resources_validated line. That event records the closure of uf
        // references a task SOURCE was validated against before its VM existed;
        // an agent's chunks arrive one at a time after the VM is up, so there is
        // no closure to have validated and an empty line would report a pass that
        // never ran.
        UF_TRY_VALUE(
            session,
            engine::EngineSession::create(
                std::move(config.frameSource),
                std::move(config.actionSink),
                *recorder,
                engine::EngineSessionConfig{
                    .liveFingerprint         = config.liveFingerprint,
                    .projectFingerprint      = spec.projectFingerprint,
                    .maximumPixelComparisons = config.maximumPixelComparisons,
                    .recognitionTimeout      = config.recognitionTimeout,
                    .maxActionFrameAge       = config.maxActionFrameAge,
                    .cancellation            = spec.cancellation,
                },
                std::move(config.ocrEngine)
            )
        );

        auto owned = std::make_unique<ExplorationSession>(
            CreateTag{},
            std::move(recorder),
            std::move(session),
            TaskContextConfig{
                .cancellation         = spec.cancellation,
                .randomSeed           = spec.seed,
                .projectRoot          = std::move(spec.projectRoot),
                .maximumReadsPerCycle = config.maximumReadsPerCycle,
                .maximumCropsPerCycle = config.maximumCropsPerCycle,
            },
            config.tracePath,
            spec.seed
        );

        // The VM is built AFTER the session owns its context, because the private
        // surface holds the context's address and the context must outlive the VM
        // (task/script-bindings.hpp). Holding both in one object with the VM
        // declared last is what makes that ordering structural rather than a rule
        // each caller has to remember.
        //
        // This is the only place ScriptTrustMode::Exploration and
        // explorationProjectGlobals() are named in the product. Every other VM in
        // this binary is a Run VM, so the wider surface exists exactly where the
        // agent front-end is and nowhere else.
        auto vm = script::Engine::create(
            script::EngineConfig{
                .cancellation      = spec.cancellation,
                .frameworkModules  = frameworkScriptModules(),
                .installHostTables = scriptHostTableInstaller(),
                .installPrivateCapabilities = scriptPrivateCapabilities(
                    owned->m_context,
                    ScriptTrustMode::Exploration
                ),
                .projectGlobals          = scriptProjectGlobals(),
                .frameworkProjectGlobals = explorationProjectGlobals(),
                .classifyRaisedError     = scriptRaisedErrorClassifier(),
            }
        );
        if (!vm)
        {
            return std::unexpected{std::move(vm).error()};
        }
        owned->m_vm = *std::move(vm);
        return owned;
    }

    auto ExplorationSession::evaluate(
        std::string_view chunk,
        std::string_view chunkName
    ) -> Result<script::ScriptValue>
    {
        if (!m_vm.has_value())
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "this exploration session has no VM; create() reports the reason "
                "a session could not be built rather than handing one back"
            );
        }

        auto result = m_vm->runValue(chunk, chunkName);

        // Sweep whatever cycle the chunk left open, WHETHER OR NOT it failed.
        //
        // A chunk is one agent-written line, and an agent that raised between
        // cycle_open and cycle_close has left the ledger holding a frame. Without
        // this, the next chunk's cycle_open is an InternalInvariant -- a
        // framework bug, latched terminal -- so one mistyped line would end the
        // session instead of costing one result line. The host owns the bracket
        // around a chunk, so the host closes it; nothing script-facing can reach
        // this, and the ordinal a swept cycle spent is never reissued, so a
        // ticket the chunk kept somehow stays dead.
        static_cast<void>(m_context.sweepOpenCycle());

        return result;
    }

    auto ExplorationSession::terminalKind() const noexcept
        -> std::optional<AutomationErrorKind>
    {
        return m_context.terminalKind();
    }

    auto ExplorationSession::finish(std::optional<Error> failure) -> TaskRunReport
    {
        // The VM dies before the closing line, so no chunk can still be running
        // when the bracket closes and nothing the VM holds outlives the context
        // it borrows.
        m_vm.reset();

        auto report = TaskRunReport{
            .seed      = m_seed,
            .tracePath = m_tracePath,
            .failure   = std::move(failure),
        };

        // A generation the host spent terminally ended there whatever the agent
        // did afterwards, exactly as for a task run and an operator session.
        if (!report.failure)
        {
            if (auto const terminal = m_context.terminalKind(); terminal.has_value())
            {
                report.failure = fail(
                    *terminal,
                    "the exploration session's generation was spent before it "
                    "ended"
                ).error();
            }
        }

        auto finishStatus = m_recorder->emit(explorationRunFinishedEvent(report));
        if (!report.failure && !finishStatus)
        {
            report.failure = std::move(finishStatus).error();
        }
        return report;
    }

    auto ExplorationSession::tracePath() const noexcept
        -> std::filesystem::path const&
    {
        return m_tracePath;
    }
}
