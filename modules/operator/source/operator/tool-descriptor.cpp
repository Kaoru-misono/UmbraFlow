#include "tool-descriptor.hpp"

#include <core/error/contracts.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <optional>
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

    auto parseRisk(std::string_view wire) noexcept -> std::optional<Risk>
    {
        constexpr auto k_risks = std::array{
            Risk::ReadOnly,
            Risk::Low,
            Risk::Medium,
            Risk::High,
            Risk::Critical,
        };
        auto const found = std::ranges::find(k_risks, wire, riskWireName);
        if (found == k_risks.end())
        {
            return std::nullopt;
        }
        return *found;
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

    auto parseToolMutability(std::string_view wire) noexcept
        -> std::optional<ToolMutability>
    {
        constexpr auto k_mutabilities = std::array{
            ToolMutability::ReadOnly,
            ToolMutability::Mutating,
        };
        auto const found = std::ranges::find(
            k_mutabilities,
            wire,
            toolMutabilityWireName
        );
        if (found == k_mutabilities.end())
        {
            return std::nullopt;
        }
        return *found;
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

    auto parseToolSurface(std::string_view wire) noexcept
        -> std::optional<ToolSurface>
    {
        constexpr auto k_surfaces = std::array{
            ToolSurface::Semantic,
            ToolSurface::Privileged,
        };
        auto const found = std::ranges::find(k_surfaces, wire, toolSurfaceWireName);
        if (found == k_surfaces.end())
        {
            return std::nullopt;
        }
        return *found;
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

    auto parseToolIdempotency(std::string_view wire) noexcept
        -> std::optional<ToolIdempotency>
    {
        constexpr auto k_idempotencies = std::array{
            ToolIdempotency::ReadSafe,
            ToolIdempotency::DeliverySafe,
            ToolIdempotency::KeyedExternal,
            ToolIdempotency::NonIdempotent,
        };
        auto const found = std::ranges::find(
            k_idempotencies,
            wire,
            toolIdempotencyWireName
        );
        if (found == k_idempotencies.end())
        {
            return std::nullopt;
        }
        return *found;
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

    auto parseTimeoutAction(std::string_view wire) noexcept
        -> std::optional<TimeoutAction>
    {
        constexpr auto k_timeoutActions = std::array{
            TimeoutAction::Reobserve,
            TimeoutAction::Reconcile,
            TimeoutAction::Stop,
        };
        auto const found = std::ranges::find(
            k_timeoutActions,
            wire,
            timeoutActionWireName
        );
        if (found == k_timeoutActions.end())
        {
            return std::nullopt;
        }
        return *found;
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
