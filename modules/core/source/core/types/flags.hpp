#pragma once

#include <compare>
#include <concepts>
#include <type_traits>
#include <utility>

namespace uf
{
    template <typename Enum>
    concept FlagEnum = (
        std::is_enum_v<Enum>
        && std::unsigned_integral<std::underlying_type_t<Enum>>
    );

    template <FlagEnum Enum>
    class Flags final
    {
    public:
        using Storage = std::underlying_type_t<Enum>;

        friend constexpr auto operator|(Flags left, Flags right) noexcept -> Flags
        {
            return left.with(right);
        }

        friend constexpr auto operator&(Flags left, Flags right) noexcept -> Flags
        {
            left.m_bits &= right.m_bits;
            return left;
        }

    private:
        Storage m_bits{};

    public:
        constexpr Flags() noexcept = default;

        constexpr explicit Flags(Enum flag) noexcept
            : m_bits{std::to_underlying(flag)}
        {
        }

        template <typename... Values>
            requires (
                sizeof...(Values) > 0
                && (std::same_as<Enum, Values> && ...)
            )
        constexpr explicit Flags(Enum first, Values... rest) noexcept
            : m_bits{static_cast<Storage>(
                (std::to_underlying(first) | ... | std::to_underlying(rest))
            )}
        {
        }

        auto operator<=>(Flags const&) const = default;

        [[nodiscard]] explicit constexpr operator bool() const noexcept { return !empty(); }

        [[nodiscard]] constexpr auto bits() const noexcept -> Storage { return m_bits; }
        [[nodiscard]] constexpr auto empty() const noexcept -> bool { return m_bits == Storage{0}; }

        [[nodiscard]]
        constexpr auto containsAll(Flags flags) const noexcept -> bool
        {
            return (m_bits & flags.m_bits) == flags.m_bits;
        }

        [[nodiscard]]
        constexpr auto containsAny(Flags flags) const noexcept -> bool
        {
            return (m_bits & flags.m_bits) != Storage{0};
        }

        constexpr auto insert(Flags flags) noexcept -> void { m_bits |= flags.m_bits; }
        constexpr auto remove(Flags flags) noexcept -> void { m_bits &= static_cast<Storage>(~flags.m_bits); }

        [[nodiscard]]
        constexpr auto with(Flags flags) const noexcept -> Flags
        {
            auto result = *this;
            result.insert(flags);
            return result;
        }

        [[nodiscard]]
        constexpr auto without(Flags flags) const noexcept -> Flags
        {
            auto result = *this;
            result.remove(flags);
            return result;
        }
    };
}
