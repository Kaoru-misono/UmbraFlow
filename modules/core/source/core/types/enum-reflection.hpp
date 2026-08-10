#pragma once

#include "core/safety/annotations.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <type_traits>

namespace uf
{
    template <typename Enum>
        requires std::is_enum_v<Enum>
    struct EnumEntry final
    {
        Enum value{};
        // Names must reference static storage; UF_REFLECT_ENUM supplies a string literal.
        std::string_view name{};
    };

    template <typename Enum>
    EnumEntry(Enum, std::string_view) -> EnumEntry<Enum>;

    template <typename Enum>
    struct EnumTraits;

    namespace detail
    {
        template <typename Type, typename Enum>
        inline constexpr bool k_isEnumEntryArray = false;

        template <typename Enum, std::size_t Size>
        inline constexpr bool k_isEnumEntryArray<std::array<EnumEntry<Enum>, Size>, Enum> = true;

        [[nodiscard]]
        constexpr auto trimEnumToken(
            std::string_view token UF_LIFETIME_BOUND
        ) noexcept -> std::string_view
        {
            while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
            {
                token.remove_prefix(1);
            }

            while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
            {
                token.remove_suffix(1);
            }

            auto const qualifier = token.rfind("::");
            if (qualifier != std::string_view::npos)
            {
                token.remove_prefix(qualifier);
                token.remove_prefix(2);
            }

            return token;
        }

        template <typename Enum, std::size_t Size>
            requires std::is_enum_v<Enum>
        [[nodiscard]]
        consteval auto makeEnumEntries(
            std::array<Enum, Size> values,
            std::string_view names
        ) noexcept -> std::array<EnumEntry<Enum>, Size>
        {
            auto entries = std::array<EnumEntry<Enum>, Size>{};
            auto remainingNames = names;

            for (auto index = std::size_t{0}; index < Size; ++index)
            {
                auto const tokenEnd = remainingNames.find(',');
                auto token = remainingNames;
                if (tokenEnd != std::string_view::npos)
                {
                    token = remainingNames.substr(0, tokenEnd);
                    remainingNames.remove_prefix(tokenEnd);
                    remainingNames.remove_prefix(1);
                }
                else
                {
                    remainingNames = {};
                }

                // Both subscripts are below Size, and this runs only under
                // constant evaluation, where a stray index is a compile error.
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
                entries[index] = EnumEntry<Enum>{values[index], trimEnumToken(token)};
            }

            return entries;
        }
    }

    template <typename Enum>
    concept ReflectedEnum = (
        std::is_enum_v<Enum>
        && requires { EnumTraits<Enum>::k_entries; }
        && detail::k_isEnumEntryArray<
            std::remove_cv_t<decltype(EnumTraits<Enum>::k_entries)>,
            Enum
        >
    );

    namespace detail
    {
        template <ReflectedEnum Enum>
        [[nodiscard]]
        consteval auto enumReflectionIsValid() noexcept -> bool
        {
            auto const& entries = EnumTraits<Enum>::k_entries;
            if (entries.empty())
            {
                return false;
            }

            for (auto index = std::size_t{0}; index < entries.size(); ++index)
            {
                // Both subscripts below stay under entries.size(), and this runs
                // only under constant evaluation, where a stray index is a
                // compile error.
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
                auto const& entry = entries[index];
                if (entry.name.empty())
                {
                    return false;
                }

                for (auto previous = std::size_t{0}; previous < index; ++previous)
                {
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
                    auto const& earlier = entries[previous];
                    if (
                        entry.value == earlier.value
                        || entry.name == earlier.name
                    )
                    {
                        return false;
                    }
                }
            }

            return true;
        }
    }

    template <ReflectedEnum Enum>
    [[nodiscard]]
    constexpr auto enumEntries() noexcept -> decltype((EnumTraits<Enum>::k_entries))
    {
        static_assert(
            detail::enumReflectionIsValid<Enum>(),
            "Enum reflection requires non-empty, unique names and values."
        );
        return EnumTraits<Enum>::k_entries;
    }

    template <ReflectedEnum Enum>
    [[nodiscard]]
    constexpr auto enumName(Enum value) noexcept -> std::optional<std::string_view>
    {
        for (auto const& entry : enumEntries<Enum>())
        {
            if (entry.value == value)
            {
                return entry.name;
            }
        }

        return std::nullopt;
    }

    template <ReflectedEnum Enum>
    [[nodiscard]]
    constexpr auto enumFromName(std::string_view name) noexcept -> std::optional<Enum>
    {
        for (auto const& entry : enumEntries<Enum>())
        {
            if (entry.name == name)
            {
                return entry.value;
            }
        }

        return std::nullopt;
    }
}

#define UF_REFLECT_ENUM(enumType, ...) \
    template <> \
    struct uf::EnumTraits<enumType> final \
    { \
        static constexpr auto k_entries = ::uf::detail::makeEnumEntries( \
            ::std::to_array<enumType>({__VA_ARGS__}), \
            #__VA_ARGS__ \
        ); \
    }; \
    static_assert( \
        ::uf::detail::enumReflectionIsValid<enumType>(), \
        "Enum reflection requires non-empty, unique names and values." \
    )
