#include "exploration-session.hpp"

#include <task/framework-bundle.hpp>
#include <task/script-bindings.hpp>
#include <task/task-context.hpp>
#include <task/task-host.hpp>
#include <task/runtime-version.hpp>

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <engine/session.hpp>

#include <script/engine.hpp>

#include <trace/event.hpp>
#include <trace/recorder.hpp>

#include <filesystem>
#include <format>
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
        // The framework version, the bundle hash and the Luau compiler version are
        // all named here: a trusted Luau framework DOES run in this session, and
        // an agent's chunk calls into `explore`.
        //
        // The task name and the source hash stay empty, and both absences are
        // accurate: a session is a sequence of chunks the agent wrote as it went,
        // none of them addressable as (project, task). What names a chunk is the
        // id its queue line carried, which reaches the trace through the chunk
        // name in a raised error rather than through this line.
        [[nodiscard]]
        auto explorationRunStartedEvent(
            std::string const& projectId,
            std::string_view frameworkBundleDigest
        ) -> trace::TraceEventSpec
        {
            return trace::TraceEventSpec{
                .eventType = "run.started",
                .audit     = trace::AuditMetadata{
                    .actor = "annotation",

                    // The bundle hash travels as a reference, not a payload
                    // field: a bare 64-character hash is indistinguishable from
                    // encoded bytes, and the trace refuses payload text that
                    // could be a smuggled frame. A reference is what a content
                    // hash was always meant to be here.
                    .references = {
                        trace::TraceReference{
                            .type = "framework_bundle",
                            .id   = std::string{frameworkBundleDigest},
                        },
                    },
                },
                .payload = trace::TypedTracePayload{
                    .fields = {
                        trace::TraceField{
                            .name  = "project_id",
                            .value = projectId,
                        },
                        trace::TraceField{
                            .name  = "framework_version",
                            .value = std::string{frameworkVersion()},
                        },
                        trace::TraceField{
                            .name  = "luau_version",
                            .value = luauRuntimeVersion(),
                        },
                    },
                },
            };
        }

        // Whether the ledger stood close enough to its ceiling that a failure
        // recorded at that moment is worth reporting as a memory failure.
        //
        // An eighth of the ceiling, chosen to be generous rather than precise: at
        // the default 64 MiB that leaves 8 MiB of headroom, more than four
        // full-frame crops, so a chunk that failed with less than that left is one
        // the ceiling was plausibly involved in. The sentence costs nothing when
        // it was not -- the figures are true either way and the original message
        // is kept verbatim in front of them -- while being wrong in the other
        // direction cost three chunks of an agent's session once.
        //
        // A VM with no ceiling never qualifies: headroomBytes reports the widest
        // value, so the comparison is false whatever `used` says.
        [[nodiscard]]
        auto nearTheCeiling(script::HeapUsage const& heap) noexcept -> bool
        {
            return heap.ceilingBytes != 0
                && heap.headroomBytes() <= heap.ceilingBytes / 8U;
        }
    }

    ExplorationSession::ExplorationSession(
        CreateTag,
        std::unique_ptr<trace::TraceRecorder> recorder,
        engine::EngineSession session,
        TaskContextConfig contextConfig,
        std::filesystem::path tracePath
    ) noexcept
        : m_recorder{std::move(recorder)}
        , m_context{std::move(session), *m_recorder, std::move(contextConfig)}
        , m_tracePath{std::move(tracePath)}
    {
    }

    auto ExplorationSession::create(
        std::unique_ptr<trace::TraceRecorder> recorder,
        engine::EngineSession session,
        ExplorationSessionSpec spec
    ) -> Result<std::unique_ptr<ExplorationSession>>
    {
        if (!recorder)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "an exploration session needs the trace recorder its ledgered "
                "caller opened; it mints none of its own"
            );
        }
        if (!spec.bindToolRuntime)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "an exploration session needs the Tool Runtime its ledgered "
                "caller admitted it under; a chunk has no other way to reach "
                "the screen, the target or the project"
            );
        }

        // Derived rather than baked -- frameworkBundleHash() hashes the embedded
        // sources together with the reserved alias, dependency depth and
        // declaration tier each module carries in framework-bundle.cpp -- so it
        // can fail in principle and is taken before anything is written.
        UF_TRY_VALUE(frameworkBundleDigest, frameworkBundleHash());

        // No run.resources_validated line. That event records the closure of uf
        // references a task SOURCE was validated against before its VM existed;
        // an agent's chunks arrive one at a time after the VM is up, so an empty
        // line would report a pass that never ran.
        auto owned = std::make_unique<ExplorationSession>(
            CreateTag{},
            std::move(recorder),
            std::move(session),
            TaskContextConfig{
                .cancellation         = spec.cancellation,
                .projectRoot          = std::move(spec.projectRoot),
                .maximumReadsPerCycle = spec.maximumReadsPerCycle,
                .maximumCropsPerCycle = spec.maximumCropsPerCycle,
            },
            std::move(spec.tracePath)
        );

        owned->m_toolRuntime = spec.bindToolRuntime(owned->m_context);
        if (!owned->m_toolRuntime)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "an exploration session's Tool Runtime binder returned no runtime"
            );
        }
        UF_TRY(
            owned->m_recorder->emit(
                explorationRunStartedEvent(spec.projectId, frameworkBundleDigest)
            )
        );

        // The VM is built AFTER the session owns its context and its seam,
        // because the one native primitive holds both addresses and both must
        // outlive the VM (task/script-bindings.hpp). Holding all three in one
        // object with the VM declared last is what makes that ordering
        // structural rather than a rule each caller has to remember.
        //
        // This is the only product path that publishes explorationProjectGlobals().
        //
        // The VM gets no runtime ceiling of its own, on purpose.
        // script::EngineConfig's maxRuntime bounds one chunk rather than the VM's
        // age, so an agent session lives as long as the agent keeps working and a
        // chunk that will not finish is still stopped, under exactly the ceiling a
        // task run answers to. What ends an ABANDONED session is `explore
        // --idle-timeout`, which measures the gap between chunks and is the only
        // clock that can tell an idle session from a busy one.
        auto vm = script::Engine::create(
            script::EngineConfig{
                .cancellation      = spec.cancellation,
                .memoryQuotaBytes  = spec.memoryQuotaBytes,
                .maxRuntime        = spec.maxScriptRuntime,
                .frameworkModules  = frameworkScriptModules(),
                .installHostTables = scriptHostTableInstaller(),
                .installPrivateCapabilities = explorationToolCapabilities(
                    owned->m_context,
                    owned->m_toolRuntime
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

        // Read the ledger BEFORE anything reclaims. A chunk that died for want
        // of memory is only recognisable at this instant: Luau raises a bare
        // "not enough memory" naming neither figure, and by the time the
        // collection below has run `used` is back at the live set, so the
        // evidence that the ceiling was involved would be gone.
        auto const atOutcome = m_vm->heapUsage();

        // Sweep whatever cycle the chunk left open, WHETHER OR NOT it failed. A
        // chunk is one agent-written line, and an agent that raised between
        // cycle_open and cycle_close has left the ledger holding a frame; without
        // this the next chunk's cycle_open is an InternalInvariant, latched
        // terminal, so one mistyped line would end the session instead of costing
        // one result line. Nothing script-facing can reach this, and the ordinal a
        // swept cycle spent is never reissued, so a ticket the chunk kept stays
        // dead.
        static_cast<void>(m_context.sweepOpenCycle());

        // Reclaim the chunk, on the same reasoning that sweeps its cycle. Nothing
        // is supposed to survive one except the project files on disk -- the
        // environment is rebuilt per chunk, the thread is discarded, and every
        // host object the chunk held is either swept above or dead with it -- so a
        // full collection here cannot reclaim anything the next chunk needs.
        //
        // It has to be here rather than in the allocator because there is no
        // emergency-GC seam to put it in: Luau throws LUA_ERRMEM the instant
        // frealloc returns null and never retries, and a retry would re-enter the
        // collector from inside the allocator callback, which that callback also
        // runs UNDER. The ceiling is therefore measured against live bytes plus
        // whatever the incremental collector has not reached, and this is where
        // the host fixes that.
        m_vm->collectGarbage();

        // A failure with the ledger against the ceiling has to SAY so. Luau's own
        // sentence is "not enough memory" and nothing else -- no figure, no hint
        // that the ceiling rather than the chunk is the subject -- and an agent
        // reading that cannot tell an out-of-memory session from a wrong chunk.
        if (!result && nearTheCeiling(atOutcome))
        {
            auto const kind = automationErrorKind(result.error())
                .value_or(AutomationErrorKind::InvalidResource);
            result = fail(
                kind,
                std::format(
                    "{} [the VM's memory ledger held {} of its {}-byte ceiling "
                    "when this chunk failed, so read this as the ceiling rather "
                    "than as the chunk: Luau refuses EVERY allocation once the "
                    "ledger is full, however small]",
                    result.error().message(),
                    atOutcome.usedBytes,
                    atOutcome.ceilingBytes
                )
            );
        }

        return result;
    }

    auto ExplorationSession::terminalKind() const noexcept
        -> std::optional<AutomationErrorKind>
    {
        // TWO LATCHES, and asking only the first is what let a spent session
        // look alive. The context latch is written by the host verbs, so it
        // covers a cancellation observed inside a primitive. A break by the
        // wall clock or the instruction budget reaches no primitive at all: it
        // latches the VM, and only script::Engine knows.
        if (auto const latched = m_context.terminalKind(); latched.has_value())
        {
            return latched;
        }
        if (m_vm.has_value() && m_vm->generationSpent())
        {
            return AutomationErrorKind::Cancelled;
        }
        return std::nullopt;
    }

    auto ExplorationSession::heapUsage() const noexcept -> script::HeapUsage
    {
        return m_vm.has_value() ? m_vm->heapUsage() : script::HeapUsage{};
    }

    auto ExplorationSession::finish(std::optional<Error> failure) -> TaskRunReport
    {
        // Asked BEFORE the VM dies, because one of the two latches lives in it.
        auto const terminal = terminalKind();

        // The VM dies before the closing line, so no chunk can still be running
        // when the bracket closes and nothing the VM holds outlives the context
        // it borrows.
        m_vm.reset();

        return closeRunBracket(
            *m_recorder,
            TaskRunReport{
                .tracePath = m_tracePath,
                .failure   = std::move(failure),
            },
            terminal,
            "the exploration session's generation was spent before it ended"
        );
    }

    auto ExplorationSession::tracePath() const noexcept
        -> std::filesystem::path const&
    {
        return m_tracePath;
    }

    auto ExplorationSession::context() noexcept -> TaskContext&
    {
        return m_context;
    }
}
