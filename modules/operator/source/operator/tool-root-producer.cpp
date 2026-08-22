#include "tool-root-producer.hpp"

#include "manifest.hpp"
#include "tool-descriptor.hpp"

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        // The risk one start proposes each of its descriptor's effect bounds
        // at. It is at or below every bound's own maximumRisk, so what limits
        // an admission is the policy the session pinned rather than a number a
        // producer chose to be generous with.
        constexpr auto k_startEffectRisk = Risk::Low;

        // The opaque project payload a start carries per effect. A start
        // proposes the effects its own descriptor declared and interprets none
        // of them, so there is nothing for it to say here; the bytes exist
        // because the minted plan is the exact document the schema defines.
        constexpr auto k_startEffectPayload = std::string_view{"{}"};

        // What a mutating start proposes: one effect per bound its own
        // descriptor declares, scoped to the controlled target this actor holds
        // the lease on, judged by the policy the session pinned. A read-only
        // start proposes none, and that is the whole of the difference between
        // the two admissions -- there is no second function and no flag.
        //
        // Approvals are deliberately empty. An approval is a human decision a
        // separate door mints, so a start that a policy requires an approval
        // for is refused by admission and the refusal is what the actor
        // renders; a producer that pre-checked policy to return a nicer error
        // would be duplicating the authority decision.
        [[nodiscard]]
        auto proposedMutation(ToolRootStart const& start)
            -> std::optional<ToolAdmissionRequest::Mutation>
        {
            auto const& descriptor = start.invocation.descriptor();
            if (descriptor.mutability != ToolMutability::Mutating)
            {
                return std::nullopt;
            }
            auto effects = std::vector<ProposedEffect>{};
            effects.reserve(descriptor.effectBounds.size());
            for (auto const& bound : descriptor.effectBounds)
            {
                effects.emplace_back(ProposedEffect{
                    .namespacedType       = bound.namespacedType,
                    .risk                 = k_startEffectRisk,
                    .scopeKind            = bound.scopeKind,
                    .scopeKey             = start.controller.controlledTargetId(),
                    .payloadSchemaHash    = bound.payloadSchemaHash,
                    .opaqueProjectPayload = std::string{k_startEffectPayload},
                });
            }
            return ToolAdmissionRequest::Mutation{
                .planAuthority = start.planAuthority,
                .effects       = std::move(effects),
                .approvals     = {},
            };
        }
    } // namespace

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
            .mutation   = proposedMutation(start),
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
