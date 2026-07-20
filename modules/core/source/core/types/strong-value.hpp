#pragma once

#include <compare>
#include <cstddef>
#include <functional>
#include <type_traits>
#include <utility>

namespace uf
{
    template <typename Tag, typename Representation>
    class StrongValue final
    {
    public:
        using ValueType = Representation;

    private:
        Representation m_value;

    public:
        constexpr explicit StrongValue(Representation value) noexcept(
            std::is_nothrow_move_constructible_v<Representation>
        )
            : m_value{std::move(value)}
        {
        }

        auto operator<=>(StrongValue const&) const = default;

        [[nodiscard]]
        constexpr auto value() const& noexcept -> Representation
            requires std::is_scalar_v<Representation>
        {
            return m_value;
        }

        [[nodiscard]]
        constexpr auto value() const& noexcept -> Representation const&
            requires (!std::is_scalar_v<Representation>)
        {
            return m_value;
        }

        [[nodiscard]]
        constexpr auto value() && noexcept(
            std::is_nothrow_move_constructible_v<Representation>
        ) -> Representation
        {
            return std::move(m_value);
        }
    };

    template <typename StrongType>
    struct StrongValueHash final
    {
        [[nodiscard]]
        auto operator()(StrongType const& value) const noexcept(
            noexcept(std::hash<typename StrongType::ValueType>{}(value.value()))
        ) -> std::size_t
        {
            using Representation = typename StrongType::ValueType;
            return std::hash<Representation>{}(value.value());
        }
    };
}
