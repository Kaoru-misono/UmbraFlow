#include "json-writer.hpp"

#include <core/types/integer.hpp>

#include <cmath>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>

namespace uf::authoring
{
    auto jsonString(std::string_view value) -> std::string
    {
        auto text = std::string{"\""};
        text.reserve(value.size() + 2U);
        for (auto const character : value)
        {
            switch (character)
            {
            case '"':
                text += "\\\"";
                break;
            case '\\':
                text += "\\\\";
                break;
            case '\n':
                text += "\\n";
                break;
            case '\r':
                text += "\\r";
                break;
            case '\t':
                text += "\\t";
                break;
            default:
                // Every other C0 control has no short escape and must not reach
                // the output raw. Bytes at or above 0x20 pass through, which
                // keeps a UTF-8 name's continuation bytes intact.
                if (static_cast<unsigned char>(character) < 0x20U)
                {
                    text += std::format(
                        "\\u{:04x}",
                        static_cast<uint32>(
                            static_cast<unsigned char>(character)
                        )
                    );
                }
                else
                {
                    text += character;
                }
                break;
            }
        }
        text += '"';
        return text;
    }

    auto jsonUnsigned(uint64 value) -> std::string
    {
        return std::format("{}", value);
    }

    auto jsonBoolean(bool value) -> std::string
    {
        return value ? std::string{"true"} : std::string{"false"};
    }

    auto jsonNull() -> std::string
    {
        return std::string{"null"};
    }

    auto jsonNumber(float value) -> std::string
    {
        if (!std::isfinite(value))
        {
            return jsonNull();
        }
        return std::format("{:.3f}", value);
    }

    auto jsonObject(std::span<JsonMember const> members) -> std::string
    {
        auto text    = std::string{"{"};
        auto leading = true;
        for (auto const& member : members)
        {
            if (!leading)
            {
                text += ',';
            }
            leading = false;
            text += jsonString(member.key);
            text += ':';
            text += member.value;
        }
        text += '}';
        return text;
    }

    auto jsonArray(std::span<std::string const> values) -> std::string
    {
        auto text    = std::string{"["};
        auto leading = true;
        for (auto const& value : values)
        {
            if (!leading)
            {
                text += ',';
            }
            leading = false;
            text += value;
        }
        text += ']';
        return text;
    }
}
