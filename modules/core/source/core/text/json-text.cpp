#include "json-text.hpp"

#include "utf8.hpp"

#include <core/types/integer.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace uf
{
    namespace
    {
        constexpr auto k_lowercaseHex = std::string_view{"0123456789abcdef"};

        // One code point as the UTF-16 code units encoding it, packed high unit
        // first so that comparing two packed values compares the unit
        // sequences. No BMP code point can equal a high surrogate, so wherever
        // two code points differ their units already differ inside this key;
        // the difference never spills into the following code point.
        [[nodiscard]]
        auto utf16SortKey(uint32 codePoint) noexcept -> uint32
        {
            if (codePoint < 0x10000U)
            {
                return codePoint << 16U;
            }

            auto const rest = codePoint - 0x10000U;
            auto const high = 0xD800U + (rest >> 10U);
            auto const low  = 0xDC00U + (rest & 0x3FFU);
            return (high << 16U) | low;
        }
    }

    auto appendJsonString(std::string& output, std::string_view value) -> void
    {
        output.push_back('"');
        for (auto const character : value)
        {
            auto const byte = static_cast<uint8>(character);
            switch (byte)
            {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\t': output += "\\t"; break;
            case '\n': output += "\\n"; break;
            case '\f': output += "\\f"; break;
            case '\r': output += "\\r"; break;
            default:
                if (byte < 0x20U)
                {
                    output += "\\u00";
                    output.push_back(k_lowercaseHex[byte >> 4U]);
                    output.push_back(k_lowercaseHex[byte & 0x0FU]);
                }
                else
                {
                    output.push_back(character);
                }
                break;
            }
        }
        output.push_back('"');
    }

    auto jsonMemberNameLess(std::string_view left, std::string_view right)
        -> bool
    {
        auto const leftScalars  = decodeUtf8Scalars(left);
        auto const rightScalars = decodeUtf8Scalars(right);
        if (!leftScalars.has_value() || !rightScalars.has_value())
        {
            if (leftScalars.has_value() != rightScalars.has_value())
            {
                return leftScalars.has_value();
            }
            return left < right;
        }

        return std::ranges::lexicographical_compare(
            *leftScalars,
            *rightScalars,
            {},
            utf16SortKey,
            utf16SortKey
        );
    }
}
