#pragma once

#include "strong-value.hpp"

#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>

namespace umbra_flow
{
    template <typename Tag, std::unsigned_integral Representation = std::uint64_t>
    using StrongId = StrongValue<Tag, Representation>;

    template <typename Tag, std::unsigned_integral Representation = std::uint64_t>
    class Generation final
    {
        Representation m_value;

        constexpr explicit Generation(Representation value) noexcept
            : m_value{value}
        {
        }

    public:
        auto operator<=>(Generation const&) const = default;

        [[nodiscard]]
        static constexpr auto initial() noexcept -> Generation
        {
            return Generation{Representation{0}};
        }

        [[nodiscard]]
        static constexpr auto fromValue(Representation value) noexcept -> Generation
        {
            return Generation{value};
        }

        [[nodiscard]] constexpr auto value() const noexcept -> Representation { return m_value; }

        [[nodiscard]]
        constexpr auto next() const noexcept -> std::optional<Generation>
        {
            if (m_value == std::numeric_limits<Representation>::max())
            {
                return std::nullopt;
            }

            return Generation{static_cast<Representation>(m_value + Representation{1})};
        }
    };
}
