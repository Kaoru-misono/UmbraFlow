#include "run.hpp"

#include "candidate-selection.hpp"
#include "file-trace-sink.hpp"
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

#include <annotation/catalog.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>
#include <engine/runtime-loader.hpp>
#include <engine/session.hpp>

#include <script/engine.hpp>

#include <task/capability-surface.hpp>
#include <task/script-validator.hpp>
#include <task/task-context.hpp>
#include <task/task-loader.hpp>

#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <memory>
#include <string>
#include <utility>

namespace uf::cli
{
    namespace
    {
        // A bound live target: the console cancellation registration and the
        // constructed engine session over that target. The cancellation is held
        // by value so it stays installed for the whole run; both run paths build
        // it identically, so the binding lives here once. The session must not
        // outlive the process-static cancellation source, which it never does --
        // both die when the owning run report scope ends.
        struct BoundTarget final
        {
            platform::ConsoleCancellation cancellation;
            engine::EngineSession         session;
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
            auto const sessionId    = SessionId{1};

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

            // 7. Wire the adapters over the resolved capabilities.
            UF_TRY_VALUE(traceSink, FileTraceSink::create(args.trace));
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
                    std::move(traceSink),
                    std::move(config)
                )
            );

            return BoundTarget{
                .cancellation = std::move(cancellation),
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
            UF_TRY_VALUE(maybeAction, pageWait.observation.findAction(actionId));

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

        // Records the task identity, script content hash, and Luau version in a
        // companion file next to the engine trace (ADR 0002). It is a sidecar, not
        // an engine-trace line, because the engine-trace/v1 schema is owned by
        // modules/engine and must not carry task fields; the versioned task-trace
        // schema is a later phase. Every field is a safe token -- the task name is
        // the loader's [A-Za-z0-9_-] segment, the hash is hex, the version is a
        // fixed spelling -- so the one-line object needs no JSON escaping.
        [[nodiscard]]
        auto writeTaskTraceRecord(
            std::filesystem::path const& tracePath,
            task::LoadedTask const& loadedTask
        ) -> Status
        {
            auto sidecar = tracePath;
            sidecar += ".task.json";

            auto stream = std::ofstream{
                sidecar,
                std::ios::binary | std::ios::trunc
            };
            if (!stream.is_open())
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format("cannot open task trace '{}'", sidecar.string())
                );
            }
            stream << std::format(
                "{{\"task\":\"{}\",\"script_hash\":\"{}\",\"luau_version\":\"{}\"}}\n",
                loadedTask.name,
                loadedTask.hash.hex(),
                task::luauRuntimeVersion()
            );
            stream.flush();
            if (!stream)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format("cannot write task trace '{}'", sidecar.string())
                );
            }
            return ok();
        }

        // The script flow: source a project-owned task by name, validate every
        // umbra reference before any VM exists, then run it on a task VM whose only
        // capability is the umbra table bound to the engine session.
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
            // (annotation-design 4).
            UF_TRY_VALUE(
                surface,
                task::CapabilitySurface::create(loaded.runtime.manifest().catalog())
            );
            UF_TRY_VALUE(loadedTask, task::loadTask(args.project, args.task));
            UF_TRY(
                task::validateScriptResources(
                    loadedTask.source,
                    loadedTask.name,
                    surface
                )
            );

            // Stamp the task identity and script hash at load time (ADR 0002).
            UF_TRY(writeTaskTraceRecord(args.trace, loadedTask));

            UF_TRY_VALUE(bound, bindTargetAndSession(args, std::move(loaded)));

            // The context owns the session and must outlive the VM that binds it,
            // so it is declared before the Engine and destroyed after it. The one
            // stop token drives both the engine (returns Cancelled) and the VM
            // interrupt (hard-breaks the task thread), so a Ctrl-C is a single
            // cancellation source, never two.
            auto context = task::TaskContext{
                std::move(bound.session),
                task::TaskContextConfig{.cancellation = bound.cancellation.token()},
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
            UF_TRY(vm.runNumber(loadedTask.source, loadedTask.name));

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
