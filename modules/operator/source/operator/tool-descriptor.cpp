#include "tool-descriptor.hpp"

#include <core/error/contracts.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace uf::operator_runtime
{
    auto riskWireName(Risk risk) noexcept -> std::string_view
    {
        switch (risk)
        {
        case Risk::ReadOnly: return "read_only";
        case Risk::Low: return "low";
        case Risk::Medium: return "medium";
        case Risk::High: return "high";
        case Risk::Critical: return "critical";
        }

        UF_UNREACHABLE_MSG("Unknown Risk value");
    }

    auto toolMutabilityWireName(ToolMutability mutability) noexcept
        -> std::string_view
    {
        switch (mutability)
        {
        case ToolMutability::ReadOnly: return "read_only";
        case ToolMutability::Mutating: return "mutating";
        }

        UF_UNREACHABLE_MSG("Unknown ToolMutability value");
    }

    auto toolSurfaceWireName(ToolSurface surface) noexcept -> std::string_view
    {
        switch (surface)
        {
        case ToolSurface::Semantic: return "semantic";
        case ToolSurface::Privileged: return "privileged";
        }

        UF_UNREACHABLE_MSG("Unknown ToolSurface value");
    }

    auto toolIdempotencyWireName(ToolIdempotency idempotency) noexcept
        -> std::string_view
    {
        switch (idempotency)
        {
        case ToolIdempotency::ReadSafe: return "read_safe";
        case ToolIdempotency::DeliverySafe: return "delivery_safe";
        case ToolIdempotency::KeyedExternal: return "keyed_external";
        case ToolIdempotency::NonIdempotent: return "non_idempotent";
        }

        UF_UNREACHABLE_MSG("Unknown ToolIdempotency value");
    }

    auto timeoutActionWireName(TimeoutAction action) noexcept -> std::string_view
    {
        switch (action)
        {
        case TimeoutAction::Reobserve: return "reobserve";
        case TimeoutAction::Reconcile: return "reconcile";
        case TimeoutAction::Stop: return "stop";
        }

        UF_UNREACHABLE_MSG("Unknown TimeoutAction value");
    }

    auto deliveryClassWithin(
        DeliveryClass claimed,
        ToolIdempotency declared
    ) noexcept -> bool
    {
        // The two enumerations are one order with one value missing from the
        // narrower of them, so the comparison is between their positions in
        // that order rather than between two unrelated codes.
        auto const claimedRank = [claimed]
        {
            switch (claimed)
            {
            case DeliveryClass::DeliverySafe:
                return std::to_underlying(ToolIdempotency::DeliverySafe);
            case DeliveryClass::KeyedExternal:
                return std::to_underlying(ToolIdempotency::KeyedExternal);
            case DeliveryClass::NonIdempotent:
                return std::to_underlying(ToolIdempotency::NonIdempotent);
            }

            UF_UNREACHABLE_MSG("Unknown DeliveryClass value");
        }();
        return claimedRank >= std::to_underlying(declared);
    }

    auto effectWithinBounds(
        ToolDescriptor const& descriptor,
        ProposedEffect const& effect
    ) -> Status
    {
        auto const found = std::ranges::find_if(
            descriptor.effectBounds,
            [&effect](EffectBound const& bound)
            {
                return bound.namespacedType == effect.namespacedType
                    && bound.scopeKind == effect.scopeKind;
            }
        );
        if (found == descriptor.effectBounds.end())
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "PlanProposal declares effect " + effect.namespacedType + " on "
                    + effect.scopeKind
                    + ", which this tool's effect_bounds do not admit"
            );
        }
        if (effect.risk > found->maximumRisk)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "PlanProposal declares effect " + effect.namespacedType + " at "
                    + std::string{riskWireName(effect.risk)}
                    + " risk, above the "
                    + std::string{riskWireName(found->maximumRisk)}
                    + " this tool's effect_bounds allow"
            );
        }
        if (effect.payloadSchemaHash != found->payloadSchemaHash)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "PlanProposal declares effect " + effect.namespacedType
                    + " under a payload schema this tool's effect_bounds do not name"
            );
        }
        return ok();
    }
}
