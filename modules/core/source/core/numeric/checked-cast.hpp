#pragma once

#include "checked-arithmetic.hpp"

#include <cmath>
#include <concepts>
#include <limits>
#include <optional>
#include <utility>

namespace umbra_flow
{
    template <CheckedInteger To, CheckedInteger From>
    [[nodiscard]]
    constexpr auto checkedCast(From value) noexcept -> std::optional<To>
    {
        if (!std::in_range<To>(value))
        {
            return std::nullopt;
        }

        return static_cast<To>(value);
    }

    template <CheckedInteger To, std::floating_point From>
    [[nodiscard]]
    inline auto checkedIntegralCast(From value) noexcept -> std::optional<To>
    {
        if (!std::isfinite(value))
        {
            return std::nullopt;
        }

        auto const wideValue = static_cast<long double>(value);
        auto const upperExclusive = std::ldexp(
            1.0L,
            std::numeric_limits<To>::digits
        );
        auto const lowerInclusive = std::signed_integral<To> ? -upperExclusive : 0.0L;

        if (
            wideValue < lowerInclusive
            || wideValue >= upperExclusive
            || std::trunc(wideValue) != wideValue
        )
        {
            return std::nullopt;
        }

        return static_cast<To>(value);
    }
}
