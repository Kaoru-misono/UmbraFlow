#include "json-string.hpp"

#include <string>
#include <string_view>

namespace uf::input_agent
{
    auto escapeJsonString(std::string_view value) -> std::string
    {
        auto output = std::string{"\""};
        output.reserve(value.size() + 2U);
        auto constexpr hex = std::string_view{"0123456789abcdef"};
        for (auto const character : value)
        {
            auto const byte = static_cast<unsigned char>(character);
            switch (byte)
            {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (byte < 0x20U)
                {
                    output += "\\u00";
                    output += hex[byte >> 4U];
                    output += hex[byte & 0x0FU];
                }
                else
                {
                    output += static_cast<char>(byte);
                }
                break;
            }
        }
        output += '"';
        return output;
    }
}
