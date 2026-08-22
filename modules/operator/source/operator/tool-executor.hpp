#pragma once

#include "ledger.hpp"
#include "tool-admission-request.hpp"

#include <functional>

namespace uf::operator_runtime
{
    // Whatever answers one call: a Framework provider reaching the world, or
    // the Project handler a registration bound to that Tool. Both receive only
    // the immutable call position, and only after the Coordinator has durably
    // crossed the dispatch boundary. The executor converts a returned domain
    // error into an exact terminal-failure payload, so no returned error
    // strands a dispatched call without a classification -- which is how a
    // handler's replay divergence becomes the durable outcome of the call it
    // was running, with the field that diverged named inside it.
    //
    // There is one provider protocol rather than a read-only and a mutating
    // one. A mutating provider may prove delivery, prove absence, or report
    // uncertainty, and an error or terminal-failure returned by a MUTATING LEAF
    // is conservatively persisted as possible because its provider code ran
    // after the durable dispatch boundary and nothing else records what it did;
    // but which shape a call is comes from the coordinate itself, so a second
    // callable type would only be a second spelling of one signature.
    using ToolProvider =
        std::function<Result<ToolCallCompletion>(ToolCallPositionIdentity const&)>;

    // The one execution seam shared by all caller adapters. It first replays a
    // recorded outcome without authority or provider execution; only the first
    // new call proceeds through admission, durable dispatch and one provider
    // invocation.
    //
    // A call left dispatching by a dead incarnation re-enters instead of
    // re-admitting, when the ledger admits a re-entry for it. Re-entry moves
    // the history revision, so the dispatch token the dead incarnation still
    // holds no longer matches the active dispatch and its terminal write is
    // refused. Which calls may re-enter is the ledger's rule and not this
    // seam's: it refuses the one shape whose interrupted dispatch may have
    // moved the world with no record of it.
    class ToolRuntimeExecutor final
    {
        OperatorCoordinator& m_coordinator;

    public:
        explicit ToolRuntimeExecutor(OperatorCoordinator& coordinator) noexcept;

        // One admission request, one provider, one durable outcome. The request
        // is the whole of what a producer supplies: read-only and mutating
        // differ in what the request carries and in how a provider's failure is
        // classified, never in which function was called.
        [[nodiscard]]
        auto invoke(
            ToolAdmissionRequest const& request,
            ToolProvider const& provider
        ) -> Result<ToolCallReplay>;
    };
}
