#pragma once

#include "tool-admission-request.hpp"
#include "tool-invocation.hpp"
#include "tool-root-producer.hpp"

#include <json/value.hpp>

#include <core/error/result.hpp>

#include <string>

namespace uf::operator_runtime
{
    // The three actor adapters of `caller independence is structural`, beside
    // the fourth producer that ruling names -- one Tool calling another, which
    // the scoped seam has always been.
    //
    // An adapter here is definitionally a translator. It resolves the actor's
    // identity, canonicalises the arguments its transport carries, and is then
    // out of the frame: policy, approvals, envelope intersection, session and
    // target authority and budgets evaluate once, inside admission, on the
    // value it built. None of them can construct anything executable, so
    // translating badly is the only mistake an adapter is able to make -- and
    // that is the class the four-way semantic fixture exists to catch, because
    // no amount of structure can see it.
    //
    // Two things are deliberately NOT an adapter's to state, and both are
    // absent from every transport type below. The caller idempotency namespace
    // is the authenticated controller's own and ToolRootProducer derives it
    // from the binding, so no adapter can hang a request key on another
    // principal's durable root. The call ordinal belongs to the issuing seam,
    // so no adapter can alias another call's position and inherit its recorded
    // outcome.
    //
    // Adding a fifth caller is adding a fourth class here. A proposed caller
    // that cannot be expressed as a translation into ToolAdmissionRequest is a
    // finding about the design and not a reason for a second path.
    //
    // Production-unreachable by construction: nothing outside a test builds
    // one. See
    // docs/decisions/2026-08-22-production-reachability-is-the-cut-invariant.md.

    // What every adapter is handed and none of them may state for itself: the
    // authenticated session this actor acts in, the run's pinned execution
    // identity, the authority a mutating start is judged under, and the Tools
    // the actor may name.
    //
    // Every member is a call-scoped borrow of state the caller owns for the
    // whole call. Nothing here is stored and this aggregate is never returned.
    //
    // No in-class initializer for any of them: a borrow has no default object
    // to name, so every run must come from construction.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct ToolActorRun final
    {
        ControllerBinding const&     controller;
        ControlLease const&          lease;
        ToolExecutionIdentity const& execution;
        OperatorPlanAuthority const& planAuthority;
        ToolStartCatalog const&      catalog;
    };

    // One tool-use block a model emitted. A model's transport delivers a parsed
    // structure rather than bytes, so what this adapter canonicalises is a
    // value.
    struct AgentToolUse final
    {
        std::string requestKey{};

        // The pre-admission request material this run is idempotent on. It is
        // the Agent's stated objective and nothing the Operator selects.
        json::Value objective{};

        std::string toolName{};
        json::Value arguments{};
    };

    // A human's Workbench or CLI command. A person delivers text, so what this
    // adapter canonicalises is bytes it must parse first -- and a person's
    // spacing, member order and number spelling are all things exact canonical
    // form would refuse, which is why the text is re-rendered rather than
    // accepted as canonical.
    struct HumanToolCommand final
    {
        std::string requestKey{};
        std::string objectiveText{};
        std::string toolName{};
        std::string argumentsText{};
    };

    // A Project starting one of its own bound entries at the top of a run.
    //
    // The entry is named the way every other Tool is named -- there is no
    // second declaration shape and no `kind` field anywhere -- and what makes
    // it startable is that this actor is admitted to start it. What this
    // adapter does state is whose entry it is: a Project may start an entry its
    // own registration bound and nothing else, which is the binding table's
    // existing question rather than a new one about what sort of thing an entry
    // is.
    struct ProjectAutomationStart final
    {
        std::string requestKey{};
        json::Value objective{};
        std::string entryToolName{};
        json::Value arguments{};
    };

    class AgentToolAdapter final
    {
        // One actor, one set of run contexts. An Agent's ordinals are its own:
        // a producer shared with another actor would number two runs from one
        // counter.
        ToolRootProducer m_producer{};

    public:
        [[nodiscard]]
        auto translate(ToolActorRun const& run, AgentToolUse const& use)
            -> Result<ToolAdmissionRequest>;
    };

    class HumanToolAdapter final
    {
        ToolRootProducer m_producer{};

    public:
        [[nodiscard]]
        auto translate(ToolActorRun const& run, HumanToolCommand const& command)
            -> Result<ToolAdmissionRequest>;
    };

    class ProjectAutomationAdapter final
    {
        ToolRootProducer m_producer{};

    public:
        // `bindings` is the registration's own binding table, borrowed for the
        // call. Holding one is already proof that every declared Tool is bound
        // and every binding names an exported entry, so this adapter asks it
        // one question and adds no rule of its own.
        [[nodiscard]]
        auto translate(
            ToolActorRun const& run,
            ProjectToolBindingTable const& bindings,
            ProjectAutomationStart const& start
        ) -> Result<ToolAdmissionRequest>;
    };
}
