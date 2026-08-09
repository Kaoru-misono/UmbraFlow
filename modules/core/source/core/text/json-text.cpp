#include "json-text.hpp"

#include <core/types/integer.hpp>

#include <string>
#include <string_view>

namespace uf
{
    namespace
    {
        constexpr auto k_lowercaseHex = std::string_view{"0123456789abcdef"};
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
}
