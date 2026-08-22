#pragma once

#include "effective-plan.hpp"
#include "ledger.hpp"
#include "project-generation.hpp"
#include "snapshot-reference.hpp"
#include "tool-admission-request.hpp"
#include "tool-executor.hpp"
#include "tool-invocation.hpp"
#include "tool-runtime.hpp"

#include <script/scoped-tool-program.hpp>

#include <core/error/result.hpp>

#include <memory>
#include <stop_token>

namespace uf::operator_runtime
{
    // The dispatch executor: everything between an admitted call and the
    // terminal durable row that answers it.
    //
    // It is the shared bottom of every caller path. An Agent, a human, a CLI
    // adapter, an automation script's root start and one Tool calling another
    // all differ in who mints the call's coordinate and what feeds the
    // authority intersection; they do not differ in what happens once a call
    // exists. That is here, once: the durable row is read, a terminal one is
    // returned without running anything, and a call that must execute gets an
    // issuing context keyed on its own position, the bound entry run with its
    // canonical arguments, the answer judged against the entry's declared
    // result schema, and the outcome written.
    //
    // Its four re-entry behaviours all fall out of that single reading rather
    // than from a case analysis:
    //
    // - a terminal parent returns its recorded result, so the handler never
    //   runs and its whole sub-tree is coordinate space nothing observes;
    // - an admitted parent whose dispatch never began dispatches for the first
    //   time against an empty history;
    // - a parent left dispatching by a dead incarnation re-enters, and its
    //   handler re-executes from the top -- children 1..j meet their recorded
    //   rows and execute nothing, and child j+1 is the first position beyond
    //   history;
    // - a zombie predecessor is excluded by the lease and the fence, which the
    //   re-entry re-reads, and by the history revision the re-entry moves.
    //
    // On EVERY entry the issuing context is fresh and numbers from 1. Resuming
    // a counter past recorded history would hand a re-executed early call a
    // fresh ordinal beyond history and execute an already-terminal effect a
    // second time, which is the exact failure replay exists to prevent. So
    // there is no persisted next-ordinal anywhere: the ordinals are re-derived
    // by re-executing, and a handler that re-derives a different call is caught
    // by the field-by-field attribute comparison at the coordinate it landed
    // on.
    //
    // Production-reachable: service::ProductLifecycle builds exactly one per
    // run, over the session's own coordinator, observation authority and plan
    // authority, and installs its own Framework providers into it. The
    // Framework Tools a scoped run reaches are still answered by the provider
    // its caller installs rather than by a registry this module owns, which is
    // what keeps the dispatcher free of any knowledge of who is answering.
    class ProjectToolDispatcher final
    {
        class State;

        std::shared_ptr<State> m_state;

        explicit ProjectToolDispatcher(std::shared_ptr<State> p_state) noexcept;

    public:
        // `coordinator` and `observations` are borrows that must outlive this
        // dispatcher and every program compiled with the seam it hands out,
        // because the seam reaches both on every child call.
        // `frameworkTools` answers the Framework Tools a scoped run reaches. In
        // production it is ProductLifecycle's own provider surface, bound to
        // the same run these three borrows name.
        //
        // `observations` must be the SAME authority the Framework observation
        // and input providers mint into and spend from. It is what turns the
        // reference bytes a script holds as data back into the authority a
        // mutating input consumes; a second authority beside the providers'
        // would recognise nothing they minted.
        //
        // `policyAuthority` is the verified authority of the session this
        // dispatcher serves, taken by value because it is one. A dispatcher
        // cannot widen anything by holding one: admission refuses an authority
        // whose registration or policy hash differs from the live session's,
        // and what it is used for is evaluating policy over effects the
        // catalog declared rather than granting any.
        //
        // Single-threaded by contract. One scoped run is synchronous on the
        // thread that dispatched it, and the seam is only ever re-entered from
        // inside that same call, so the live-run table is mutated by one thread
        // and never observed by another.
        [[nodiscard]]
        static auto create(
            OperatorCoordinator& coordinator,
            SnapshotObservationAuthority& observations,
            OperatorPolicyAuthority policyAuthority,
            ToolProvider frameworkTools
        ) -> Result<ProjectToolDispatcher>;

        ProjectToolDispatcher(ProjectToolDispatcher const&) noexcept = default;
        ProjectToolDispatcher(ProjectToolDispatcher&&) noexcept      = default;
        auto operator=(ProjectToolDispatcher const&) noexcept
            -> ProjectToolDispatcher& = default;
        auto operator=(ProjectToolDispatcher&&) noexcept
            -> ProjectToolDispatcher& = default;
        ~ProjectToolDispatcher() = default;

        // The one native seam every program this dispatcher drives is compiled
        // with. It is bound once, at compile time, and therefore carries ZERO
        // run state: one program serves every run of its registration, so the
        // seam resolves WHICH run a call belongs to from the coordinate's
        // durable parent position and from nothing else. That is what makes a
        // single compilation per registration sound.
        //
        // The callable owns everything it reaches, so it is safe to store for
        // as long as the program lives.
        [[nodiscard]] auto toolRuntimeSeam() const -> script::ToolRuntimeInvoke;

        // Dispatch one call of one Tool this program binds.
        //
        // The request is the same value every other producer builds, and it is
        // borrowed for the duration of the call, which strictly outlives the
        // run it starts. A call the run's own context issued carries no
        // delegation grant and a handler's child carries the grant that
        // handler is running under; admission is what judges which of the two
        // this is.
        //
        // A run started here may issue mutating children. Its proposed effects
        // come from the child descriptor's own bounds and this dispatcher's
        // controlled target -- see proposedToolMutation -- so the script that
        // named the Tool states none of them.
        [[nodiscard]]
        auto dispatch(
            ProjectGenerationHandle const& program,
            ToolAdmissionRequest const& request,
            std::stop_token cancellation
        ) -> Result<ToolCallReplay>;
    };
}
