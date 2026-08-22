#include "tool-actor-adapters.hpp"

#include <core/error/result.hpp>

#include <string_view>
#include <utility>

namespace uf::operator_runtime
{
    namespace
    {
        // The canonical form of one transported value. Every adapter reaches
        // canonical bytes through this one rendering, so an actor's transport
        // decides what it delivers and never how the bytes it delivers are
        // spelled -- which is the divergence the four-way fixture would
        // otherwise have to catch after the fact.
        [[nodiscard]]
        auto canonicalise(json::Value const& value) -> Result<CanonicalJson>
        {
            return CanonicalJson::parseExact(json::canonicalBytes(value));
        }

        // The same, for a transport that delivers text. Parsing first is the
        // whole difference: a person's spacing, member order and number
        // spelling are refused by exact canonical form, and re-rendering is
        // what makes a typed document mean what its author meant rather than
        // what its bytes happened to be.
        [[nodiscard]]
        auto canonicaliseText(std::string_view text) -> Result<CanonicalJson>
        {
            UF_TRY_VALUE(parsed, json::parse(text));
            return canonicalise(parsed);
        }

        // The tail every adapter shares once its transport is out of the frame:
        // the catalog that owns the name validates it against the arguments,
        // and the run's own producer mints the root coordinate.
        //
        // `producer` is mutable because advancing this run's ordinals is what
        // this call is for: the seam owns the child index and a producer that
        // did not move would hand the next start the same coordinate.
        [[nodiscard]]
        auto startOf(
            ToolActorRun const& run,
            ToolRootProducer& producer,
            std::string toolName,
            CanonicalJson arguments,
            std::string requestKey,
            CanonicalJson objective
        ) -> Result<ToolAdmissionRequest>
        {
            UF_TRY_VALUE(
                invocation,
                run.catalog.validate(std::move(toolName), std::move(arguments))
            );
            return producer.start(ToolRootStart{
                .controller      = run.controller,
                .lease           = run.lease,
                .execution       = run.execution,
                .policyAuthority = run.policyAuthority,
                .invocation      = invocation,
                .requestKey      = std::move(requestKey),
                .requestPreimage = std::move(objective),
            });
        }
    } // namespace

    auto AgentToolAdapter::translate(
        ToolActorRun const& run,
        AgentToolUse const& use
    ) -> Result<ToolAdmissionRequest>
    {
        UF_TRY_VALUE(objective, canonicalise(use.objective));
        UF_TRY_VALUE(arguments, canonicalise(use.arguments));
        return startOf(
            run,
            m_producer,
            use.toolName,
            std::move(arguments),
            use.requestKey,
            std::move(objective)
        );
    }

    auto HumanToolAdapter::translate(
        ToolActorRun const& run,
        HumanToolCommand const& command
    ) -> Result<ToolAdmissionRequest>
    {
        UF_TRY_VALUE(objective, canonicaliseText(command.objectiveText));
        UF_TRY_VALUE(arguments, canonicaliseText(command.argumentsText));
        return startOf(
            run,
            m_producer,
            command.toolName,
            std::move(arguments),
            command.requestKey,
            std::move(objective)
        );
    }

    auto ProjectAutomationAdapter::translate(
        ToolActorRun const& run,
        ProjectToolBindingTable const& bindings,
        ProjectAutomationStart const& start
    ) -> Result<ToolAdmissionRequest>
    {
        // A Project starts entries its own registration bound. The refusal is
        // about whose entry it is and never about what sort of entry it is:
        // the table answers the same question a dispatcher asks it, so a name
        // this registration never bound is another party's Tool rather than a
        // Tool of the wrong kind.
        UF_TRY(bindings.entryPointFor(start.entryToolName));
        UF_TRY_VALUE(objective, canonicalise(start.objective));
        UF_TRY_VALUE(arguments, canonicalise(start.arguments));
        return startOf(
            run,
            m_producer,
            start.entryToolName,
            std::move(arguments),
            start.requestKey,
            std::move(objective)
        );
    }
}
