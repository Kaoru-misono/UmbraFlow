#include "key.hpp"

#include "error.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <string_view>

namespace uf
{
    namespace
    {
        inline constexpr auto k_functionKeyCount = uint32{12};

        [[nodiscard]]
        auto rejectKeyName(std::string_view name) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "key name must be an uppercase \"A\"-\"Z\", \"0\"-\"9\", or "
                    "\"F1\"-\"F12\", got \"{}\"",
                    name
                )
            );
        }

        // The function-key number `name` prints, or nullopt when it is not a
        // function key at all. A leading zero and a number outside 1..12 are both
        // refused here rather than clamped, so "F0" and "F13" name nothing and
        // "F" stays the letter key.
        [[nodiscard]]
        constexpr auto functionKeyNumber(
            std::string_view name
        ) noexcept -> std::optional<uint32>
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
    }

    auto KeyName::create(std::string_view name) -> Result<KeyName>
    {
        auto const isFunctionKey = functionKeyNumber(name).has_value();
        if (!isFunctionKey)
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
