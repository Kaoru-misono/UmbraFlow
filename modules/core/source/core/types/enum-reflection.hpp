#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <type_traits>

namespace umbra_flow
{
    template <typename Enum>
        requires std::is_enum_v<Enum>
    struct EnumEntry final
    {
        Enum value;
        std::string_view name;
    };

    template <typename Enum>
    EnumEntry(Enum, std::string_view) -> EnumEntry<Enum>;

    template <typename Enum>
    struct EnumTraits;

    namespace detail
    {
        template <typename Type, typename Enum>
        inline constexpr bool isEnumEntryArray = false;

        template <typename Enum, std::size_t Size>
        inline constexpr bool isEnumEntryArray<std::array<EnumEntry<Enum>, Size>, Enum> = true;

        [[nodiscard]]
        constexpr auto trimEnumToken(std::string_view token) noexcept -> std::string_view
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
                token.remove_prefix(qualifier + 2);
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
            auto tokenBegin = std::size_t{0};

            for (auto index = std::size_t{0}; index < Size; ++index)
            {
                auto tokenEnd = names.find(',', tokenBegin);
                if (tokenEnd == std::string_view::npos)
                {
                    tokenEnd = names.size();
                }

                entries[index] = EnumEntry<Enum>{
                    values[index],
                    trimEnumToken(names.substr(tokenBegin, tokenEnd - tokenBegin))
                };
                tokenBegin = tokenEnd < names.size() ? tokenEnd + 1 : tokenEnd;
            }

            return entries;
        }
    }

    template <typename Enum>
    concept ReflectedEnum = (
        std::is_enum_v<Enum>
        && requires { EnumTraits<Enum>::entries; }
        && detail::isEnumEntryArray<
            std::remove_cv_t<decltype(EnumTraits<Enum>::entries)>,
            Enum
        >
    );

    namespace detail
    {
        template <ReflectedEnum Enum>
        [[nodiscard]]
        consteval auto enumReflectionIsValid() noexcept -> bool
        {
            auto const& entries = EnumTraits<Enum>::entries;
            if (entries.empty())
            {
                return false;
            }

            for (auto index = std::size_t{0}; index < entries.size(); ++index)
            {
                if (entries[index].name.empty())
                {
                    return false;
                }

                for (auto previous = std::size_t{0}; previous < index; ++previous)
                {
                    if (
                        entries[index].value == entries[previous].value
                        || entries[index].name == entries[previous].name
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
    constexpr auto enumEntries() noexcept -> decltype((EnumTraits<Enum>::entries))
    {
        static_assert(
            detail::enumReflectionIsValid<Enum>(),
            "Enum reflection requires non-empty, unique names and values."
        );
        return EnumTraits<Enum>::entries;
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

#define UMBRA_FLOW_REFLECT_ENUM(enumType, ...) \
    template <> \
    struct umbra_flow::EnumTraits<enumType> final \
    { \
        static constexpr auto entries = ::umbra_flow::detail::makeEnumEntries( \
            ::std::to_array<enumType>({__VA_ARGS__}), \
            #__VA_ARGS__ \
        ); \
    }; \
    static_assert( \
        ::umbra_flow::detail::enumReflectionIsValid<enumType>(), \
        "Enum reflection requires non-empty, unique names and values." \
    )
