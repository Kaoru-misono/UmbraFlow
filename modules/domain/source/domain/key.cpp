#include "key.hpp"

#include "error.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace uf
{
    namespace
    {
        inline constexpr auto k_functionKeyCount = uint32{12};

        // create() copies a name into k_maxKeyNameBytes of storage without
        // re-checking its length, so a named key longer than that would write
        // past the array. Fail the build instead.
        static_assert(
            std::ranges::all_of(
                k_namedKeys,
                [](std::string_view name)
                {
                    return !name.empty() && name.size() <= k_maxKeyNameBytes;
                }
            ),
            "every named key must fit the storage a KeyName reserves"
        );

        // The named keys as a refusal should print them. Rendered from the set
        // rather than written out, so the message cannot fall behind it.
        [[nodiscard]]
        auto namedKeyList() -> std::string
        {
            auto text = std::string{};
            for (auto const name : k_namedKeys)
            {
                if (!text.empty())
                {
                    text += ", ";
                }
                text += std::format("\"{}\"", name);
            }
            return text;
        }

        // Names the whole vocabulary, including its case rule. "uppercase" used
        // to sit in front of "A"-"Z" alone, which read as a rule about letters
        // and left an author who typed "enter" with no way to see why it was
        // refused.
        [[nodiscard]]
        auto rejectKeyName(std::string_view name) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "key name must be \"A\"-\"Z\", \"0\"-\"9\", \"F1\"-\"F12\", "
                    "or one of {}, spelled in uppercase throughout, got \"{}\"",
                    namedKeyList(),
                    name
                )
            );
        }
    }

    auto functionKeyNumber(std::string_view name) noexcept -> std::optional<uint32>
    {
        if (!name.starts_with('F'))
        {
            return std::nullopt;
        }
        auto const digits = name.substr(1U);
        if (digits.empty() || digits.size() > 2U || digits.front() == '0')
        {
            return std::nullopt;
        }

        auto number = uint32{0};
        for (auto const digit : digits)
        {
            if (digit < '0' || digit > '9')
            {
                return std::nullopt;
            }
            number = (number * 10U) + static_cast<uint32>(digit - '0');
        }
        if (number < 1U || number > k_functionKeyCount)
        {
            return std::nullopt;
        }
        return number;
    }

    auto KeyName::create(std::string_view name) -> Result<KeyName>
    {
        // Three families, tested as three memberships rather than as a chain of
        // literal comparisons: a name belongs to the named set, to the function
        // keys, or is a single letter or digit.
        auto const isNamedKey    = std::ranges::contains(k_namedKeys, name);
        auto const isFunctionKey = functionKeyNumber(name).has_value();
        if (!isNamedKey && !isFunctionKey)
        {
            if (name.size() != 1U)
            {
                return rejectKeyName(name);
            }
            auto const character = name.front();
            auto const isLetter  = character >= 'A' && character <= 'Z';
            auto const isDigit   = character >= '0' && character <= '9';
            if (!isLetter && !isDigit)
            {
                return rejectKeyName(name);
            }
        }

        auto text = std::array<char, k_maxKeyNameBytes>{};
        for (auto index = std::size_t{0}; index < name.size(); ++index)
        {
            text[index] = name[index];
        }
        return KeyName{text, static_cast<uint8>(name.size())};
    }

    auto KeyName::value() const noexcept -> std::string_view
    {
        return std::string_view{m_text.data(), m_length};
    }
}
