#pragma once

#include <task/cycle-ledger.hpp>
#include <task/task-context.hpp>
#include <task/task-host.hpp>

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/ids.hpp>
#include <domain/key.hpp>
#include <domain/space.hpp>

#include <engine/session.hpp>

#include <trace/recorder.hpp>

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
    // How many deadline handles one operator session may mint. A ceiling exists
    // because the session has to remember every deadline it handed out -- an
    // operator addresses one by an id over a text protocol, not by holding a value
    // -- and a table that only grows is one an operator can make arbitrarily large
    // by asking. It sits far above any plausible session.
    inline constexpr auto k_maxOperatorDeadlines = std::size_t{1024};

    // The operator front-end's consumer of the private capability surface: a
    // SIBLING of the trusted Luau framework, not a route into it. The guarantee
    // layer sits below Luau -- the observation-cycle ledger, the four-requisite
    // click authorization, the fingerprint check and the trace all live in C++
    // behind TaskContext -- so an operator gets exactly the primitives and exactly
    // the refusals a task gets, because both call the same TaskContext. It is
    // emphatically NOT a way to run Luau: there is no chunk, no source and no
    // string that becomes code, only C++ methods taking scalars and names.
    //
    // This session runs no Luau, so it cannot name elements or pages; model-grade
    // access comes back through the exploration environment, and a second, weaker
    // page model must not be invented in C++ here
    // (docs/plans/2026-07-31-script-owned-page-model.md 9).
    //
    // There is deliberately no `raise`, `emit` or `random`. `emit` would put a
    // second author on the framework.* events, which the trace stream validator
    // refuses outright on an operator stream; `random` exists only because the
    // Luau sandbox removed math.random, and an operator has its own.
    //
    // Luau's primitives take opaque userdata and an operator has only text, so the
    // session hands out integer ids and keeps the real values. The ids confer
    // nothing, and staleness is decided by the LEDGER rather than here: an ordinal
    // is turned back into a ticket the ledger judges on exactly the terms it
    // judges the Luau path.
    //
    // Lifetime: non-movable, because the TaskContext it owns borrows the recorder
    // it owns, and the context's address must stay fixed for the ports below it.
    // Created through TaskHost::startOperatorSession, which latches the
    // generation's front-end claim.
    //
    // NOT thread-safe: every verb runs on the owning thread.
    class OperatorSession final
    {
        // Makes the constructor reachable from create() and from nowhere else.
        struct CreateTag final
        {
        };

        // Declared first and held through a unique_ptr: the engine session and the
        // context both borrow it (see their trace lifetime contracts), so it must
        // outlive them and keep a stable address.
        std::unique_ptr<trace::TraceRecorder> m_recorder;

        TaskContext m_context;

        // The last ticket cycleOpen minted, so an ordinal an operator supplies can
        // be turned back into a ticket for the ledger to judge. A
        // default-constructed ticket carries generation 0, which no ledger ever
        // stamps (CycleLedger mints from 1), so a session that has opened no cycle
        // produces a ticket the LEDGER refuses rather than one this class had to
        // refuse on its own authority.
        std::optional<CycleTicket> m_ticket{};

        std::vector<MonotonicInstant> m_deadlines{};

        std::filesystem::path m_tracePath;
        uint64                m_seed;

        // The ticket for `ordinal`, for the ledger to judge. See m_ticket.
        [[nodiscard]] auto ticketFor(uint64 ordinal) const noexcept -> CycleTicket;

    public:
        // Everything one operator session needs that is a property of the loaded
        // project rather than of the desktop. The ports and budgets arrive
        // separately in a TaskRunConfig.
        struct Spec final
        {
            std::string projectId{};

            // The geometry the generation's page model was authored at, which the
            // engine's compatibility refusal compares the bound target against.
            ProjectFingerprint projectFingerprint;

            std::stop_token cancellation{};
        };

        OperatorSession(
            CreateTag,
            std::unique_ptr<trace::TraceRecorder> recorder,
            engine::EngineSession session,
            TaskContextConfig contextConfig,
            std::filesystem::path tracePath,
            uint64 seed
        ) noexcept;

        OperatorSession(OperatorSession const&) = delete;
        OperatorSession(OperatorSession&&) = delete;
        auto operator=(OperatorSession const&) -> OperatorSession& = delete;
        auto operator=(OperatorSession&&) -> OperatorSession& = delete;

        ~OperatorSession() = default;

        // Opens the run bracket and binds the ports. Everything fallible happens
        // here, so a session that exists has run.started in its trace and an
        // engine session over a bound target. `config` and `spec` are taken by
        // value because this is a move-in ownership boundary: the two ports end up
        // inside the session.
        [[nodiscard]]
        static auto create(
            TaskRunConfig config,
            Spec spec,
            TaskRunId runId,
            GenerationId generationId
        ) -> Result<std::unique_ptr<OperatorSession>>;

        // Closes the run bracket and reports how the session ended, on the same
        // terms a task run reports: an empty `failure` is a clean end. It is the
        // caller's last call on this session. `failure` is taken by value because
        // an Error has exactly one owner and the returned report carries it onward.
        [[nodiscard]]
        auto finish(std::optional<Error> failure) -> TaskRunReport;

        // Each verb below is the primitive of the same name on the Luau surface --
        // same arguments, same failure modes, same task.native_call line -- with
        // the opaque handles replaced by the ids described above. Nothing here
        // composes two primitives and nothing here decides policy.

        [[nodiscard]] auto cycleOpen() -> Result<uint64>;

        // Whether there was a frame to release. Idempotent, exactly as the primitive
        // is.
        [[nodiscard]] auto cycleClose(uint64 cycleOrdinal) -> Result<bool>;

        [[nodiscard]] auto key(uint64 cycleOrdinal, KeyName keyName) -> Status;

        [[nodiscard]] auto settle(MonotonicInstant::Duration duration) -> Status;

        [[nodiscard]]
        auto deadline(MonotonicInstant::Duration duration) -> Result<uint64>;

        // Whether budget remains. False is the deadline expiring, which ends the
        // caller's wait loop.
        [[nodiscard]]
        auto wait(
            uint64 deadlineId,
            MonotonicInstant::Duration interval
        ) -> Result<bool>;

        [[nodiscard]]
        auto tracePath() const noexcept UF_LIFETIME_BOUND
            -> std::filesystem::path const&;
    };
}
