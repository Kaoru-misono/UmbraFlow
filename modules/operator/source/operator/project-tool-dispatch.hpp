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

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

namespace uf::operator_runtime
{
    // The release of an input a Tool call engaged and left held.
    //
    // A callable protocol rather than a handle on anything, because the
    // dispatcher reaches no action sink and must not learn to: the child call
    // that pressed is the only thing that knows how to lift, so it hands the
    // lift back and the frame that owns the press holds it until that frame
    // closes.
    using HeldInputRelease = std::move_only_function<Status()>;

    // The close of an observation frame a Tool call opened.
    //
    // A callable for HeldInputRelease's reason: the dispatcher reaches no Host
    // and must not learn to. The call that opened the frame is the only thing
    // that knows how to close it, so it leaves the close here and the dispatcher
    // runs it when that call's scope exits.
    using ObservationFrameClose = std::move_only_function<Status()>;

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
    // canonical arguments, and the outcome written.
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

        // Hands the run anchored on `holdingCall` the release of an input a
        // child call has just engaged and left held.
        //
        // THE FRAME THAT LIFTS A HELD INPUT IS THE TOOL CALL THAT OWNS IT, not
        // the leaf call that pressed. A leaf that released before returning
        // could hold nothing while anything looked at the screen, and looking at
        // the screen while the button is down is the entire capability; see
        // docs/decisions/2026-08-24-an-authoring-session-is-a-first-class-tool-session.md.
        // The engaging leaf therefore returns with the button still down and
        // leaves its release here, and the run lifts it on every exit path.
        //
        // The parent/child structure this rests on is the LEDGER'S CALL TREE and
        // not the call stack: `holdingCall` is a durable position, the children
        // that follow are ordinary un-nested calls, and nothing is re-entered. A
        // release the target refused is reported rather than swallowed, because
        // a target that will not take one is exactly what an operator has to be
        // told about.
        //
        // Refused when no run is anchored at `holdingCall`, and when that run
        // already holds a release: one release lifts every held input at once,
        // so a second would share the first one's act and neither engagement
        // could say which one ended.
        [[nodiscard]]
        auto attachHeldInput(
            ContentHash const& holdingCall,
            HeldInputRelease release
        ) -> Status;

        // Opens the observation frame the Tool call at `frameCall` owns.
        //
        // AN OBSERVATION FRAME IS THE SCOPE OF THE CALL THAT OPENED IT
        // (docs/decisions/2026-08-24-an-observation-frame-is-the-scope-of-its-call.md).
        // The frame is anchored on the observe call's OWN durable position,
        // which is why root and nested are one shape: a held input is a leaf
        // whose effect spills outward onto sibling calls and therefore needs an
        // outer owner, while a frame's effect acts only inward on the calls
        // inside its own scope, so it is already its own owner and a root
        // position lacks nothing.
        //
        // AT MOST ONE FRAME IS OPEN AT A TIME, and a second is REFUSED BY NAME
        // rather than superseding the first. Supersession would dress the
        // single-slot implementation up as a method and let a distant call
        // decide whether a local measurement succeeds; the refusal instead names
        // the holding call, the limit, and what exceeded it. It is also what
        // keeps the Host's own one-cycle invariant a framework bug rather than
        // something a Project can write.
        [[nodiscard]]
        auto engageObservationFrame(
            ContentHash const& frameCall,
            ObservationFrameClose close
        ) -> Status;

        // Closes the observation frame this dispatcher holds, unless the frame
        // it holds is `inherited` -- the one that was already open when the
        // scope now exiting was entered.
        //
        // Answers ok() when nothing is held, and when what is held is the
        // inherited frame, so a scope can run this unconditionally on every exit
        // path without ever closing a frame belonging to a scope outside it.
        // That is the whole of "the frame closes on ANY exit path": the caller
        // does not have to know which path it is on.
        //
        // Not a scope guard, for the reason attachHeldInput's release is not:
        // one would have to swallow the close's own failure, and a Host that
        // will not release a frame is exactly what an operator has to be told
        // about.
        [[nodiscard]]
        auto closeObservationFrameOpenedInside(
            std::optional<ContentHash> const& inherited
        ) -> Status;

        // The durable position of the Tool call whose observation frame is open,
        // or nothing when none is. It is what a scope reads on entry so it can
        // tell an inherited frame from one opened inside it.
        [[nodiscard]]
        auto heldObservationFrame() const -> std::optional<ContentHash>;

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
