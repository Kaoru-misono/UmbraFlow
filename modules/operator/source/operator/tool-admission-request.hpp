#pragma once

#include "controller.hpp"
#include "effective-plan.hpp"
#include "ledger.hpp"
#include "tool-descriptor.hpp"
#include "tool-invocation.hpp"
#include "tool-runtime.hpp"

#include <optional>
#include <vector>

namespace uf::operator_runtime
{
    // The one internal admission-request value, and the only thing
    // OperatorCoordinator::admitToolCall accepts.
    //
    // Per `caller independence is structural` an adapter is definitionally a
    // translator from transport specifics into this value: it resolves the
    // actor identity, canonicalises the arguments the coordinate was minted
    // from, and is then out of the frame. Policy, approvals, envelope
    // intersection, session and target authority, and budgets evaluate once,
    // inside admission, on this value. A fifth caller is a fifth translator; a
    // proposed caller that cannot be expressed as a translation into this value
    // is a finding about the design rather than a reason for a second path.
    //
    // What makes that unbypassable is not this type's access control. This is
    // an ordinary transport aggregate any producer may fill in. It is safe
    // because two of its members can only come from the runtime:
    // `call` is minted by ToolCallIssuingContext, whose position factory is
    // private to it, and `delegation` is minted by the Coordinator over a
    // parent whose durable row is already dispatching. Nothing a producer can
    // build out of transport bytes is executable, so translating badly is the
    // only mistake a producer is able to make -- which is exactly the class the
    // four-way semantic fixture exists to catch, and exactly the class this
    // structure is blind to.
    //
    // Two pairs of these fields have to agree -- a delegation grant with a
    // child coordinate, and a mutation with a mutating descriptor -- and both
    // are nonetheless judged by admission rather than by a refusing factory
    // here. The reason is that the authoritative form of each question is a
    // durable read: whether that grant is live, whether the parent it names is
    // still dispatching, and whether those effects clear policy are answers
    // only the ledger holds. A factory could restate the local half of one
    // agreement, and would then have split one answer across two places for a
    // producer to look. Admission states all of it, once, and what a producer
    // renders is the refusal that came back.
    //
    // No in-class initializer for controller, lease, root and call: an actor,
    // a lease and a coordinate must all come from construction, and there is no
    // default any of them could carry. Three of the four have no default
    // constructor at all, so the aggregate has none either and cannot be left
    // indeterminate.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct ToolAdmissionRequest final
    {
        // What a mutating call proposes, and what judges it. It is absent for
        // a read-only Tool and required for a mutating one, which is the whole
        // of the read-only/mutating distinction in this value: the mutability
        // itself is read off the descriptor the coordinate carries, so no
        // producer can state one the catalog disagrees with.
        //
        // The plan authority is present because policy is evaluated rather than
        // named. A producer cannot widen anything by supplying its own:
        // admission refuses an authority whose registration or policy hash
        // differs from the live session's.
        //
        // No in-class initializer for the authority: OperatorPlanAuthority has
        // no default state, and a mutation with no authority to judge it is not
        // a value this type should be able to hold.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        struct Mutation final
        {
            OperatorPlanAuthority planAuthority;

            std::vector<ProposedEffect>    effects{};
            std::vector<ToolApprovalGrant> approvals{};
        };

        ControllerBinding        controller;
        ControlLease             lease;
        ToolRootRequestIdentity  root;
        ToolCallPositionIdentity call;

        std::optional<Mutation>            mutation{};
        std::optional<ToolDelegationGrant> delegation{};

        // Read off the descriptor inside the coordinate, never stated beside
        // it. A second spelling of the mutability is a second thing that can be
        // wrong, and the catalog is already the one that decides.
        [[nodiscard]] auto requiredMutability() const noexcept -> ToolMutability;

        // A call the run's own issuing context issued, as opposed to a
        // handler's child. It is decided by the coordinate alone: a root call's
        // parent coordinate is the run's root request, and a child's is the
        // handler's own position row. There is no third case and no absent
        // parent.
        [[nodiscard]] auto isRootPositioned() const -> bool;
    };
}
