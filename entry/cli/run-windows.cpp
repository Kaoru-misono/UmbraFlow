#include "run.hpp"

#include "candidate-selection.hpp"
#include "name-resolution.hpp"
#include "platform/controller-action-sink.hpp"
#include "platform/wgc-frame-source.hpp"
#include "platform/windows-console-cancellation.hpp"
#include "platform/windows-target-geometry.hpp"

#include <controller/capture.hpp>
#include <controller/discovery.hpp>
#include <controller/dpi.hpp>
#include <controller/input.hpp>
#include <controller/target.hpp>
#include <core/error/result.hpp>

#include <annotation/resource.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>
#include <engine/runtime-loader.hpp>
#include <engine/session.hpp>

#include <script/engine.hpp>

#include <task/capability-surface.hpp>
#include <task/run-trace.hpp>
#include <task/script-validator.hpp>
#include <task/task-context.hpp>
#include <task/task-loader.hpp>

#include <trace/file-sink.hpp>
#include <trace/recorder.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace uf::cli
{
    namespace
    {
        // P0 runs one task per process, so the run and the script-layer
        // generation are both the first of their kind. Stage 1d's TaskHost hands
        // out real per-run values; until then these are the honest constants for
        // a one-run process rather than placeholders for a missing source.
        constexpr auto k_singleRunId        = TaskRunId{1};
        constexpr auto k_singleGenerationId = GenerationId{1};

        // A bound live target: the console cancellation registration, the run's
        // trace recorder, and the constructed engine session over that target.
        // The cancellation is held by value so it stays installed for the whole
        // run; both run paths build it identically, so the binding lives here
        // once. The session must not outlive the process-static cancellation
        // source, which it never does -- both die when the owning run report
        // scope ends.
        //
        // The recorder is held through a unique_ptr so its address survives this
        // struct being moved out of bindTargetAndSession: the session stores a
        // borrow of it (see engine/session.hpp's trace lifetime contract), and
        // declaring it before the session makes the session die first. This is
        // the composition root that owns the run's single evidence stream; stage
        // 1d relocates exactly this ownership into TaskHost.
        struct BoundTarget final
        {
            platform::ConsoleCancellation         cancellation;
            std::unique_ptr<trace::TraceRecorder> recorder;
            engine::EngineSession                 session;
        };

        // Declares DPI awareness, installs Ctrl-C cancellation, resolves the
        // target by selector substring, builds the live fingerprint and capture
        // session, wires the frame source / action sink / trace sink, and creates
        // the engine session over `loaded`. This is the desktop-binding half both
        // run paths share verbatim; the smoke path and the script path differ only
        // in what they drive the session with afterward.
        [[nodiscard]]
        auto bindTargetAndSession(
            RunArgs const& args,
            engine::LoadedRuntime loaded
        ) -> Result<BoundTarget>
        {
            // 1. Declare per-monitor DPI awareness through the controller.
            UF_TRY(ensurePerMonitorAwareV2());

            // 2. Install Ctrl-C cancellation feeding the engine's stop token.
            UF_TRY_VALUE(cancellation, platform::ConsoleCancellation::install());

            // 3. Enumerate, pick by selector substring, and resolve the target.
            UF_TRY_VALUE(candidates, enumerateCandidates());
            UF_TRY_VALUE(chosen, selectCandidate(candidates, args.selector));
            auto const selector = TargetSelector{}.withWindowHandle(chosen.handle());
            UF_TRY_VALUE(resolved, resolveTarget(candidates, selector));

            auto const client       = resolved.clientSize();
            auto const windowHandle = resolved.windowHandle();
            auto const generation   = resolved.currentGeneration();
            auto const sessionId    = CaptureSessionId{1};

            // 4. Build the live fingerprint from resolved geometry and target DPI.
            // A mismatch against the manifest fingerprint makes the engine fail
            // closed.
            auto const dpi = chosen.dpi().value();
            UF_TRY_VALUE(
                liveFingerprint,
                annotation::ProjectFingerprint::create(
                    client.width(),
                    client.height(),
                    dpi,
                    dpi
                )
            );

            // 5. Create the capture session from resolved geometry.
            UF_TRY_VALUE(origin, platform::clientOriginDesktop(windowHandle));
            UF_TRY_VALUE(
                geometry,
                ClientGeometry::create(
                    origin,
                    static_cast<float>(client.width()),
                    static_cast<float>(client.height())
                )
            );
            UF_TRY_VALUE(
                session,
                WgcCaptureSession::create(
                    windowHandle,
                    sessionId,
                    generation,
                    geometry
                )
            );

            // 6. Create the delivery target for background input.
            UF_TRY_VALUE(
                delivery,
                DeliveryTarget::create(
                    windowHandle,
                    sessionId,
                    generation,
                    client.width(),
                    client.height()
                )
            );

            // 7. Wire the adapters over the resolved capabilities. The trace file
            // opens only once the target is bound, so a run that never found its
            // window leaves no evidence file behind.
            UF_TRY_VALUE(traceSink, trace::FileTraceSink::create(args.trace));
            auto recorder = std::make_unique<trace::TraceRecorder>(
                std::move(traceSink),
                k_singleRunId,
                k_singleGenerationId
            );
            auto frameSource = std::make_unique<platform::WgcFrameSource>(
                std::move(session)
            );
            auto actionSink = std::make_unique<platform::ControllerActionSink>(
                delivery
            );

            // 8. Build the engine session over the resolved capabilities.
            auto config = engine::EngineSessionConfig{
                .liveFingerprint         = liveFingerprint,
                .maximumPixelComparisons = args.budget,
                .recognitionTimeout      = args.recognitionTimeout,
                .maxActionFrameAge       = args.maxFrameAge,
                .cancellation            = cancellation.token(),
            };
            UF_TRY_VALUE(
                engineSession,
                engine::EngineSession::create(
                    std::move(loaded),
                    std::move(frameSource),
                    std::move(actionSink),
                    *recorder,
                    std::move(config)
                )
            );

            return BoundTarget{
                .cancellation = std::move(cancellation),
                .recorder     = std::move(recorder),
                .session      = std::move(engineSession),
            };
        }

        // The single-step smoke flow: wait for the page, find the action, and act.
        // Unchanged from the original runProduct beyond drawing its bound session
        // from the shared helper.
        [[nodiscard]]
        auto runSmokeFlow(
            RunArgs const& args,
            engine::LoadedRuntime loaded
        ) -> Result<RunReport>
        {
            UF_TRY_VALUE(
                pageId,
                resolvePageName(loaded.runtime.manifest().catalog(), args.page)
            );
            UF_TRY_VALUE(
                actionId,
                resolveActionName(loaded.runtime.manifest().catalog(), args.action)
            );

            UF_TRY_VALUE(bound, bindTargetAndSession(args, std::move(loaded)));

            UF_TRY_VALUE(
                pageWait,
                bound.session.waitForPage(pageId, args.timeout, args.pollInterval)
            );
            UF_TRY_VALUE(
                maybeAction,
                bound.session.findAction(pageWait.observation, actionId)
            );

            auto report = RunReport{
                .pageName   = args.page,
                .actionName = args.action,
                .tracePath  = args.trace.string(),
            };
            if (!maybeAction)
            {
                report.actionDelivered = false;
                return report;
            }

            UF_TRY_VALUE(
                receipt,
                bound.session.act(
                    std::move(pageWait.observation),
                    pageWait.page,
                    *maybeAction
                )
            );
            report.actionDelivered = true;
            report.clickClientX    = receipt.clickPoint.x();
            report.clickClientY    = receipt.clickPoint.y();
            return report;
        }

        // The script flow: source a project-owned task by name, validate every
        // umbra reference before any VM exists, then run it on a task VM whose only
        // capability is the umbra table bound to the engine session. One
        // umbraflow-trace/v1 stream records the run's identity, its validated
        // resource closure, every recognition and action step, every host-API
        // return, and how it ended -- all under one sequence, run id and
        // generation id.
        [[nodiscard]]
        auto runScriptFlow(
            RunArgs const& args,
            engine::LoadedRuntime loaded
        ) -> Result<RunReport>
        {
            // Build the capability surface, load the task, and validate it before
            // binding any target -- so a bad project, an unsafe or missing task
            // name, or a disallowed umbra reference fails without touching the
            // desktop, and, crucially, before any VM is created
            // (annotation-design 4). The validated resource report feeds the
            // ResourcesValidated trace event.
            UF_TRY_VALUE(
                surface,
                task::CapabilitySurface::create(loaded.runtime.manifest().catalog())
            );
            UF_TRY_VALUE(loadedTask, task::loadTask(args.project, args.task));
            UF_TRY_VALUE(
                resourceReport,
                task::validateScriptResources(
                    loadedTask.source,
                    loadedTask.name,
                    surface
                )
            );

            // Capture the project identity before the runtime moves into the
            // session, so TaskStarted can record which project the task ran against.
            auto const projectId = std::string{
                loaded.runtime.manifest().catalog().projectId().value()
            };

            UF_TRY_VALUE(bound, bindTargetAndSession(args, std::move(loaded)));

            // The fixed seed the deterministic RNG draws from is recorded in the
            // trace so a run reproduces on replay; the host uses the stable default
            // until a per-run seed source lands.
            auto const seed = task::k_defaultRandomSeed;

            // The run identity opens the stream, before the VM exists, so every
            // later event is attributable to this exact task build -- including
            // the framework version and bundle hash, which task::runStartedEvent
            // reads from this binary rather than taking from here.
            UF_TRY(
                bound.recorder->emit(
                    task::runStartedEvent(
                        task::RunStartSpec{
                            .projectId  = projectId,
                            .taskName   = loadedTask.name,
                            .sourceHash = loadedTask.hash.hex(),
                            .seed       = seed,
                        }
                    )
                )
            );
            UF_TRY(
                bound.recorder->emit(
                    task::runResourcesValidatedEvent(resourceReport)
                )
            );

            // The context owns the session and borrows the same recorder, and must
            // outlive the VM that binds it, so it is declared before the Engine and
            // destroyed after it. The one stop token drives both the engine
            // (returns Cancelled) and the VM interrupt (hard-breaks the task
            // thread), so a Ctrl-C is a single cancellation source, never two.
            auto context = task::TaskContext{
                std::move(bound.session),
                *bound.recorder,
                task::TaskContextConfig{
                    .cancellation = bound.cancellation.token(),
                    .randomSeed   = seed,
                },
            };

            auto vmConfig = script::EngineConfig{
                .cancellation      = bound.cancellation.token(),
                .installHostTables = surface.installer(context),
            };
            UF_TRY_VALUE(vm, script::Engine::create(vmConfig));

            // The script's numeric return carries no success meaning of its own,
            // so it is discarded: a Tier B or Tier C failure surfaces as an error
            // here (Cancelled maps to the cancellation exit code at the boundary),
            // and a clean return means the task ran to completion.
            auto runResult = vm.runNumber(loadedTask.source, loadedTask.name);

            // Record how the generation ended before surfacing the outcome. The
            // outcome mapping itself lives in task::runFinishedEvent, where a test
            // can reach it.
            auto finishStatus = context.emitTrace(
                task::runFinishedEvent(runResult ? nullptr : &runResult.error())
            );

            // The run's own failure takes precedence over a trace-emit failure, so
            // surface it first; a failed TaskFinished emit only fails the run when
            // the run itself succeeded.
            UF_TRY(std::move(runResult));
            UF_TRY(std::move(finishStatus));

            return RunReport{
                .scriptMode = true,
                .taskName   = loadedTask.name,
                .scriptHash = loadedTask.hash.hex(),
                .tracePath  = args.trace.string(),
            };
        }
    }

    auto runProduct(RunArgs const& args) -> Result<RunReport>
    {
        // Load the project before touching the desktop, so a bad project fails
        // fast without binding any target. The task field selects the run path:
        // the script path when a task was named, the single-step smoke path
        // otherwise. parseRunArguments already rejected supplying both or neither.
        UF_TRY_VALUE(loaded, engine::loadRuntimeProject(args.project));
        if (!args.task.empty())
        {
            return runScriptFlow(args, std::move(loaded));
        }
        return runSmokeFlow(args, std::move(loaded));
    }

    // Reads the process-lifetime console cancellation source, which outlives the
    // ConsoleCancellation registration installed during a run, so a Ctrl-C remains
    // observable at the exit-code boundary after the run has returned.
    auto runCancellationRequested() noexcept -> bool
    {
        return platform::ConsoleCancellation::stopRequested();
    }
}
