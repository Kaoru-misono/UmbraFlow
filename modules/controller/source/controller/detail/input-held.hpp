#pragma once

#include "controller/input.hpp"

#include <functional>
#include <utility>
#include <vector>

namespace uf::controller_detail
{
    struct HeldInputsAccess final
    {
        [[nodiscard]]
        static auto ensureTarget(
            HeldInputs const& held,
            DeliveryTarget const& target
        ) -> Status
        {
            return held.ensureTarget(target);
        }

        [[nodiscard]]
        static auto onKeyDown(
            HeldInputs& held,
            DeliveryTarget const& target,
            KeyInput key
        ) -> Result<bool>
        {
            return held.onKeyDown(target, key);
        }

        [[nodiscard]]
        static auto onKeyUp(
            HeldInputs& held,
            DeliveryTarget const& target,
            KeyInput key
        ) -> Result<bool>
        {
            return held.onKeyUp(target, key);
        }

        [[nodiscard]]
        static auto onPointerDown(
            HeldInputs& held,
            DeliveryTarget const& target,
            PointerButton button,
            ClientPixel pixel
        ) -> Status
        {
            return held.onPointerDown(target, button, pixel);
        }

        [[nodiscard]]
        static auto onPointerUp(
            HeldInputs& held,
            DeliveryTarget const& target,
            PointerButton button
        ) -> Result<bool>
        {
            return held.onPointerUp(target, button);
        }

        [[nodiscard]]
        static auto releaseAll(
            HeldInputs& held,
            DeliveryTarget const& target,
            std::move_only_function<Status(HeldInput)> postUp
        ) -> std::vector<ReleaseOutcome>
        {
            return held.releaseAll(target, std::move(postUp));
        }
    };
}
