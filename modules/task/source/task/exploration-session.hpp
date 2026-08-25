#pragma once

#include <task/task-context.hpp>
#include <task/task-host.hpp>

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <engine/session.hpp>

#include <script/scoped-tool-program.hpp>

#include <trace/recorder.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

namespace uf::task
{
    // The agent front-end: a live target, a project, and one Luau chunk at a time.
    //
    // A third front-end rather than a mode of the other two, because an agent
    // sends CODE chunk by chunk and reads what came back before writing the next
    // (docs/archive/plans/2026-08-01-three-layers-and-agent-operator.md 3). That
    // is an INTERACTION SHAPE -- a queue of code against a single call -- and it
    // is the whole of what makes this a separate front end.
    //
    // IT IS NOT A SECOND ENVIRONMENT, and there is no trust split here. Each
    // chunk is a root scoped Tool program: it imports the same scoped SDK modules
    // as a Project Tool handler and can call any Framework or Project Tool in the
    // session's pinned combined catalog. Every call is recorded in the same
    // ledger under this session's own identity and admitted or refused by the
    // same Operator policy. What differs between callers is which grants that
    // policy carries -- nothing else, and in particular no property of this type
    // (docs/decisions/2026-08-24-there-is-no-annotation-phase.md, and
    // docs/decisions/2026-08-24-policy-is-the-axis-and-observation-holds-a-frame.md
    // V1).
    //
    // It owns NEITHER its trace recorder's identity, NOR its engine session, NOR
    // its Tool Runtime: all three are built by the ledgered caller that admitted
    // this run and handed over at create(). What that buys is that an
    // interactive session's trace stream carries the Operator's own session id
    // and SessionManifest hash, its engine is bound to the RuntimeModel that
    // session pinned, and its Tool calls occupy durable positions under that
    // session's root request -- rather than an identity this class derived for
    // itself off the project directory's name
    // (docs/decisions/2026-08-24-policy-is-the-axis-and-observation-holds-a-frame.md
    // V5).
    //
    // One chunk is one bracket. Each evaluate() runs its chunk under a project
    // environment built fresh for it, so globals one chunk writes never reach the
    // next, and the session sweeps any observation frame the chunk left open. What
    // survives between chunks is everything the HOST owns: the ledger's ordinals,
    // the template store, the trace sequence, and the target binding.
    //
    // Lifetime: non-movable, because the context borrows the recorder and every
    // per-chunk VM borrows the context-backed Tool Runtime while it evaluates.
    // Created through TaskHost::startExplorationSession, which latches the
    // generation's front-end claim.
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

        // Empty until create() validates the shared scoped Framework closure.
        // Each evaluate() runs one fresh VM; this object owns the pinned module
        // and catalog bytes plus the one Tool Runtime they all call through.
        std::optional<script::ScopedToolSession> m_program{};

        std::filesystem::path m_tracePath;

    public:
        ExplorationSession(
            CreateTag,
            std::unique_ptr<trace::TraceRecorder> recorder,
            engine::EngineSession session,
            TaskContextConfig contextConfig,
            std::filesystem::path tracePath
        ) noexcept;

        ExplorationSession(ExplorationSession const&) = delete;
        ExplorationSession(ExplorationSession&&) = delete;
        auto operator=(ExplorationSession const&) -> ExplorationSession& = delete;
        auto operator=(ExplorationSession&&) -> ExplorationSession& = delete;

        ~ExplorationSession() = default;

        // Opens the run bracket and boots the VM over the recorder and engine
        // session the caller admitted this run with. Everything fallible happens
        // here, so a session that exists has run.started in its trace, a bound
        // target, and an environment ready to run a chunk.
        //
        // Reached in production only through TaskHost::startExplorationSession,
        // which latches the generation's front-end claim.
        [[nodiscard]]
        static auto create(
            std::unique_ptr<trace::TraceRecorder> recorder,
            engine::EngineSession session,
            ExplorationSessionSpec spec
        ) -> Result<std::unique_ptr<ExplorationSession>>;

        // Runs one agent-supplied chunk and reports what it returned. `chunkName`
        // labels the chunk in compile diagnostics and in a raised error's
        // traceback; neither view is stored.
        //
        // A chunk that fails is an ORDINARY outcome: the agent reads the failure
        // and sends another chunk. A cancellation or a spent generation is not,
        // and surfaces as the failure kind the caller ends the session on.
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

        // The VM's memory ledger as it stands, which after an evaluate() is the
        // reading AFTER that chunk's reclamation -- the live figure, not the
        // garbage the chunk left behind. The caller reports it on every result
        // line so an agent watches itself approach the ceiling; a session with no
        // VM reports an empty readout.
        [[nodiscard]]
        auto heapUsage() const noexcept -> script::HeapUsage;

        // Closes the run bracket and reports how the session ended, on the same
        // terms a task run and an operator session report.
        [[nodiscard]]
        auto finish(std::optional<Error> failure) -> TaskRunReport;

        [[nodiscard]]
        auto tracePath() const noexcept UF_LIFETIME_BOUND
            -> std::filesystem::path const&;

        // The context this session's chunks run against, for the ledgered
        // caller that started it. A Tool call issued through that caller has to
        // reach the SAME engine session the chunks do, or a session would be
        // observing one screen and recording another; borrowing it here is what
        // makes that structural rather than a rule two composition roots have to
        // agree on. The borrow lives only as long as this session does.
        [[nodiscard]] auto context() noexcept UF_LIFETIME_BOUND -> TaskContext&;
    };
}
