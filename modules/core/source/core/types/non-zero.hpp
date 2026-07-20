#pragma once

#include "core/numeric/checked-arithmetic.hpp"

#include <compare>
#include <optional>

namespace umbra_flow
{
    template <CheckedInteger Value>
    class NonZero final
    {
        Value m_value;

        constexpr explicit NonZero(Value value) noexcept
            : m_value{value}
        {
        }

    public:
        auto operator<=>(NonZero const&) const = default;

        [[nodiscard]]
        static constexpr auto create(Value value) noexcept -> std::optional<NonZero>
        {
            if (value == Value{0})
            {
                return std::nullopt;
            }

            return NonZero{value};
        }

        [[nodiscard]] constexpr auto value() const noexcept -> Value { return m_value; }
    };
}
