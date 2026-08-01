#pragma once

#include <task/task-context.hpp>
#include <task/task-host.hpp>

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <engine/session.hpp>

#include <script/engine.hpp>

#include <trace/recorder.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace uf::task
{
    // The agent front-end: a live target, a project, and one Luau chunk at a
    // time.
    //
    // WHAT MAKES IT THE THIRD FRONT-END RATHER THAN A MODE OF THE OTHER TWO. A
    // task runs one script the host loaded from the project; an operator sends
    // commands that are not code at all; an agent sends CODE, chunk by chunk,
    // and looks at what came back before writing the next one. That loop --
    // capture, look, click, capture again -- is the workflow an annotation
    // session actually has (docs/plans/2026-08-01-three-layers-and-agent-
    // operator.md 3), and neither of the other two front-ends can express it: a
    // task cannot be written before the model exists, and an operator protocol
    // cannot compose two verbs without growing a second copy of the framework.
    //
    // THE SECOND ENVIRONMENT LIVES HERE. The VM this owns is booted with the
    // Exploration private surface -- the run surface plus `cycle_crop` and
    // `probe` -- and publishes two framework modules a run VM does not, `explore`
    // and `scribe`. That pair of differences is the whole of the trust split; see
    // task/script-bindings.hpp and task/framework-bundle.hpp for why each half is
    // where it is. Everything else about the VM is identical to a task's,
    // deliberately: an agent that measured the system under weaker guarantees
    // would be measuring a system the product does not ship.
    //
    // ONE CHUNK IS ONE BRACKET. Each evaluate() runs its chunk under a project
    // environment built fresh for it, so globals one chunk writes never reach the
    // next; and the session sweeps any observation cycle the chunk left open, so
    // a chunk that raised mid-cycle costs its own line and not the session. What
    // does survive between chunks is everything the HOST owns: the ledger's
    // ordinals, the template store, the trace sequence, and the target binding.
    //
    // Lifetime: non-movable, because the VM borrows the context, the context
    // borrows the recorder, and both addresses must stay fixed. Created through
    // TaskHost::startExplorationSession, which latches the generation's
    // front-end claim, so a generation cannot end up with both this and a task
    // run.
    //
    // NOT thread-safe: every verb runs on the owning thread.
    class ExplorationSession final
    {
        // Makes the constructor reachable from create() and nowhere else.
        struct CreateTag final
        {
        };

        // Declared first and held through a unique_ptr: the engine session, the
        // context and the VM all borrow it, so it must outlive them and keep a
        // stable address.
        std::unique_ptr<trace::TraceRecorder> m_recorder;

        TaskContext m_context;

        // Empty until create() boots it, and destroyed before m_context because
        // members die in reverse declaration order -- which is the lifetime
        // contract task/script-bindings.hpp states for the private surface.
        std::optional<script::Engine> m_vm{};

        std::filesystem::path m_tracePath;
        uint64                m_seed;

    public:
        // Everything one exploration session needs that is a property of the
        // loaded project rather than of the desktop.
        struct Spec final
        {
            std::string projectId{};

            ProjectFingerprint projectFingerprint;

            // The directory project_read and project_write are confined to,
            // which is the generation's own project root. An exploration session
            // needs it where an operator session did not: `scribe` writes
            // template assets and rewrites the model file, and both are
            // project_write calls that must not be able to name the rest of the
            // disk.
            std::filesystem::path projectRoot{};

            // This session's RNG seed, drawn by the host and recorded in
            // run.started. The exploration surface carries `random`, so a seed
            // is a real replay input here rather than the zero an operator
            // session records for having no randomness at all.
            uint64 seed{};

            std::stop_token cancellation{};
        };

        ExplorationSession(
            CreateTag,
            std::unique_ptr<trace::TraceRecorder> recorder,
            engine::EngineSession session,
            TaskContextConfig contextConfig,
            std::filesystem::path tracePath,
            uint64 seed
        ) noexcept;

        ExplorationSession(ExplorationSession const&) = delete;
        ExplorationSession(ExplorationSession&&) = delete;
        auto operator=(ExplorationSession const&) -> ExplorationSession& = delete;
        auto operator=(ExplorationSession&&) -> ExplorationSession& = delete;

        ~ExplorationSession() = default;

        // Opens the run bracket, binds the ports and boots the VM. Everything
        // fallible happens here, so a session that exists has run.started in its
        // trace, a bound target, and an environment ready to run a chunk.
        //
        // `config` and `spec` are taken by value because this is a move-in
        // ownership boundary: the ports end up inside the session.
        [[nodiscard]]
        static auto create(
            TaskRunConfig config,
            Spec spec,
            TaskRunId runId,
            GenerationId generationId
        ) -> Result<std::unique_ptr<ExplorationSession>>;

        // Runs one agent-supplied chunk and reports what it returned.
        //
        // `chunkName` labels the chunk in compile diagnostics and in a raised
        // error's traceback, so the agent's own id for the line is what it reads
        // back. Neither view is stored.
        //
        // A chunk that fails is an ORDINARY outcome: the agent reads the failure
        // and sends another chunk. What is not ordinary is a cancellation or a
        // spent generation, and those surface as the failure kind the caller
        // ends the session on.
        [[nodiscard]]
        auto evaluate(
            std::string_view chunk,
            std::string_view chunkName
        ) -> Result<script::ScriptValue>;

        // Whether the generation has been spent, and under which kind. The
        // caller ends the session on it rather than sending another chunk into a
        // VM that will refuse every primitive.
        [[nodiscard]]
        auto terminalKind() const noexcept -> std::optional<AutomationErrorKind>;

        // Closes the run bracket and reports how the session ended, on the same
        // terms a task run and an operator session report.
        [[nodiscard]]
        auto finish(std::optional<Error> failure) -> TaskRunReport;

        [[nodiscard]]
        auto tracePath() const noexcept UF_LIFETIME_BOUND
            -> std::filesystem::path const&;
    };
}
