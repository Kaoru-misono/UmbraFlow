#include "tool-admission-request.hpp"

#include <string>
#include <utility>

namespace uf::operator_runtime
{
    namespace
    {
        // The risk one call proposes each of its descriptor's effect bounds at.
        constexpr auto k_proposedEffectRisk = Risk::Low;

        // The opaque project payload one proposed effect carries.
        constexpr auto k_proposedEffectPayload = std::string_view{"{}"};
    } // namespace

    auto ToolAdmissionRequest::requiredMutability() const noexcept
        -> ToolMutability
    {
        return call.descriptor().mutability;
    }

    auto ToolAdmissionRequest::isRootPositioned() const -> bool
    {
        return call.parentIdentity() == root.identity();
    }

    auto proposedToolMutation(
        ValidatedToolInvocation const& invocation,
        OperatorPlanAuthority const& planAuthority,
        std::string_view controlledTargetId
    ) -> std::optional<ToolAdmissionRequest::Mutation>
    {
        auto const& descriptor = invocation.descriptor();
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
                .risk                 = k_proposedEffectRisk,
                .scopeKind            = bound.scopeKind,
                .scopeKey             = std::string{controlledTargetId},
                .payloadSchemaHash    = bound.payloadSchemaHash,
                .opaqueProjectPayload = std::string{k_proposedEffectPayload},
            });
        }
        return ToolAdmissionRequest::Mutation{
            .planAuthority = planAuthority,
            .effects       = std::move(effects),
            .approvals     = {},
        };
    }
}
