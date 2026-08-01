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
    // How many deadline handles one operator session may mint.
    //
    // A ceiling exists because the session has to remember every deadline it
    // handed out -- an operator addresses one by an id over a text protocol, not by
    // holding a value -- and a table that only grows is one an operator can make
    // arbitrarily large by asking. It sits far above any plausible session: a
    // deadline bounds a wait, and an operator that has started a thousand waits in
    // one session will not be helped by a thousand and first.
    inline constexpr auto k_maxOperatorDeadlines = std::size_t{1024};

    // The operator front-end's consumer of the private capability surface.
    //
    // WHAT THIS IS, AND WHAT IT IS NOT. It is a SIBLING of the trusted Luau
    // framework, not a route into it. The guarantee layer sits below Luau: the
    // observation-cycle ledger, the four-requisite click authorization, the
    // fingerprint check and the trace all live in C++ behind TaskContext, and
    // modules/task/runtime/ctx.luau is one consumer of it. This is a second
    // consumer at the same level, so an operator gets exactly the primitives a task
    // gets and exactly the refusals a task gets, because both call the same
    // TaskContext.
    //
    // It is emphatically NOT a way to run Luau. There is no chunk, no source and no
    // string that becomes code: every verb below is a C++ method taking scalars and
    // names. The sandbox removes every eval route from a task VM on purpose, and
    // nothing here restores one.
    //
    // WHAT AN OPERATOR MAY NAME, AND WHAT IT NO LONGER CAN. It named elements and
    // pages while the page model was C++, and it does not any more: the model is a
    // Luau object built out of the project file, and this session runs no Luau
    // (docs/plans/2026-07-31-script-owned-page-model.md 9). So resolving a page,
    // finding an element and clicking one are gone from this surface, together
    // with the C++ verbs behind them. What is left is what needs no model --
    // observing, keying, and the time verbs.
    //
    // That is a real loss of operator reach and it is meant to be temporary: the
    // exploration environment of work order 4b is where model-grade access comes
    // back, on top of the layer-two model rather than beside it. Nothing here
    // invents a replacement in the meantime, because a second, weaker page model
    // in C++ is exactly what this wave retired.
    //
    // WHAT IS DELIBERATELY ABSENT BESIDES. There is no `raise`, no `emit` and no
    // `random`. The first two exist so the trusted framework can fail on its own
    // terms and record its own structure; an operator has no structure of its own
    // to record and reports a failure by writing a result line. `random` exists
    // because the Luau sandbox removed math.random and a script has no other
    // source of it; an operator has its own. Admitting `emit` in particular would
    // put a second author on the framework.* events, which the trace stream
    // validator now refuses outright on an operator stream.
    //
    // HANDLES OVER A TEXT PROTOCOL. Luau's primitives take opaque userdata; an
    // operator has only text, so the session hands out integer ids and keeps the
    // real values. The ids confer nothing, and staleness is decided by the LEDGER
    // rather than here: a cycle ordinal the operator supplies is turned back into a
    // ticket and handed to the ledger, which reports StaleObservation for exactly
    // the cycles it reports it for on the Luau path.
    //
    // Lifetime: non-movable, because the TaskContext it owns borrows the recorder it
    // owns, and the context's address must stay fixed for the ports below it.
    // Created through TaskHost::startOperatorSession, which is also what latches the
    // generation's front-end claim, so a generation cannot end up with both this and
    // a task run.
    //
    // NOT thread-safe: every verb runs on the owning thread.
    class OperatorSession final
    {
        // Makes the constructor reachable from create() and from nowhere else: it is
        // a public constructor taking a private type, so the session is still built
        // through the factory that opened its trace while std::make_unique remains
        // the only allocation.
        struct CreateTag final
        {
        };

        // Declared first and held through a unique_ptr: the engine session and the
        // context both borrow it (see their trace lifetime contracts), so it must
        // outlive them and keep a stable address.
        std::unique_ptr<trace::TraceRecorder> m_recorder;

        TaskContext m_context;

        // The last ticket cycleOpen minted, kept so an ordinal an operator supplies
        // can be turned back into a ticket for the ledger to judge. A
        // default-constructed ticket carries generation 0, which no ledger ever
        // stamps (CycleLedger mints from 1), so a session that has opened no cycle
        // still produces a ticket the ledger refuses rather than one this class had
        // to refuse on its own authority.
        std::optional<CycleTicket> m_ticket{};

        std::vector<MonotonicInstant> m_deadlines{};

        std::filesystem::path m_tracePath;
        uint64                m_seed;

        // The ticket for `ordinal`, for the ledger to judge. See m_ticket.
        [[nodiscard]] auto ticketFor(uint64 ordinal) const noexcept -> CycleTicket;

    public:
        // Everything one operator session needs that is a property of the loaded
        // project rather than of the desktop. TaskHost fills it from the generation
        // it is about to bind; the ports and budgets arrive separately in a
        // TaskRunConfig, which is the same move-in ownership boundary a task run
        // uses.
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
        // here, so a session that exists has run.started in its trace and an engine
        // session over a bound target.
        //
        // `config` and `spec` are taken by value because this is a move-in
        // ownership boundary: the two ports end up inside the session.
        [[nodiscard]]
        static auto create(
            TaskRunConfig config,
            Spec spec,
            TaskRunId runId,
            GenerationId generationId
        ) -> Result<std::unique_ptr<OperatorSession>>;

        // Closes the run bracket and reports how the session ended, on the same
        // terms a task run reports: an empty `failure` is a clean end, and every
        // other ending is described by the error that caused it. It is the caller's
        // last call on this session.
        //
        // `failure` is taken by value because an Error has exactly one owner: this is
        // where the caller hands over the failure that ended its loop, and the
        // returned report carries it onward.
        [[nodiscard]]
        auto finish(std::optional<Error> failure) -> TaskRunReport;

        // LAYER ONE. Each verb below is the primitive of the same name on the Luau
        // surface -- same arguments, same failure modes, same task.native_call line
        // -- with the opaque handles replaced by the ids described above. Nothing
        // here composes two primitives and nothing here decides policy.

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
