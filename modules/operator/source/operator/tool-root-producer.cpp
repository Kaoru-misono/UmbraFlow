#include "tool-root-producer.hpp"

#include "manifest.hpp"

#include <string>
#include <utility>

namespace uf::operator_runtime
{
    ToolStartCatalog::ToolStartCatalog(
        FrameworkToolCatalogOwner framework,
        ProjectToolCatalogSchemaOwner project
    )
        : m_framework{std::move(framework)}
        , m_project{std::move(project)}
    {
    }

    auto ToolStartCatalog::create(ProjectToolCatalogSchemaOwner project)
        -> Result<ToolStartCatalog>
    {
        UF_TRY_VALUE(framework, FrameworkToolCatalogOwner::create());
        return ToolStartCatalog{std::move(framework), std::move(project)};
    }

    auto ToolStartCatalog::projectRegistrationHash() const -> ContentHash
    {
        return m_project.projectRegistrationHash();
    }

    auto ToolStartCatalog::validate(
        std::string toolName,
        CanonicalJson canonicalArgs
    ) const -> Result<ValidatedToolInvocation>
    {
        auto const framework =
            validateToolNameOwnership(toolName, k_frameworkToolNamespace);
        return framework.has_value()
            ? m_framework.validate(std::move(toolName), std::move(canonicalArgs))
            : m_project.validate(std::move(toolName), std::move(canonicalArgs));
    }

    auto ToolRootProducer::contextFor(
        ToolRootRequestIdentity const& root,
        ToolExecutionIdentity const& execution
    ) -> ToolCallIssuingContext&
    {
        auto const opened = m_contexts.find(root.identity());
        if (opened != m_contexts.end())
        {
            return opened->second;
        }
        auto const created = m_contexts.emplace(
            root.identity(),
            ToolCallIssuingContext::forRoot(root, execution)
        );
        return created.first->second;
    }

    auto ToolRootProducer::requestFor(
        ToolRootStart const& start,
        ToolRootRequestIdentity root,
        ToolCallPositionIdentity call
    ) const -> Result<ToolAdmissionRequest>
    {
        // No delegation grant. This is a call the run's own context issued, and
        // a grant exists only for a child call under a dispatching handler --
        // which admission decides from the coordinate rather than from anything
        // stated here.
        return ToolAdmissionRequest{
            .controller = start.controller,
            .lease      = start.lease,
            .root       = std::move(root),
            .call       = std::move(call),
            .mutation   = proposedToolMutation(
                start.invocation,
                start.policyAuthority,
                start.controller.controlledTargetId()
            ),
        };
    }

    auto ToolRootProducer::start(ToolRootStart const& start)
        -> Result<ToolAdmissionRequest>
    {
        UF_TRY_VALUE(
            root,
            ToolRootRequestIdentity::create(
                start.controller.controllerId(),
                start.requestKey,
                start.requestPreimage
            )
        );
        auto& context = contextFor(root, start.execution);
        UF_TRY_VALUE(call, context.issue(start.invocation));
        return requestFor(start, std::move(root), std::move(call));
    }

    auto ToolRootProducer::startAgainstObservation(
        ToolRootStart const& start,
        SnapshotObservationReference const& observation
    ) -> Result<ToolAdmissionRequest>
    {
        UF_TRY_VALUE(
            root,
            ToolRootRequestIdentity::create(
                start.controller.controllerId(),
                start.requestKey,
                start.requestPreimage
            )
        );
        auto& context = contextFor(root, start.execution);
        UF_TRY_VALUE(
            call,
            context.issueAgainstObservation(start.invocation, observation)
        );
        return requestFor(start, std::move(root), std::move(call));
    }
}
