#include "json-scan.hpp"

#include "json-text.hpp"

#include <core/types/integer.hpp>

#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace uf::trace
{
    namespace
    {
        // One hex digit's value, or nullopt when the byte is not one. Used only
        // for the \u00XX form, which is the only escape `escapeJsonString`
        // produces beyond the seven named ones.
        [[nodiscard]]
        auto hexDigit(char character) noexcept -> std::optional<uint32>
        {
            if (character >= '0' && character <= '9')
            {
                return static_cast<uint32>(character - '0');
            }
            if (character >= 'a' && character <= 'f')
            {
                return static_cast<uint32>(character - 'a') + 10U;
            }
            if (character >= 'A' && character <= 'F')
            {
                return static_cast<uint32>(character - 'A') + 10U;
            }
            return std::nullopt;
        }
    }

    auto skipString(
        std::string_view line,
        std::size_t      start
    ) noexcept -> std::optional<std::size_t>
    {
        auto index = start + 1U;
        while (index < line.size())
        {
            if (line[index] == '\\')
            {
                index += 2U;
                continue;
            }
            if (line[index] == '"')
            {
                return index + 1U;
            }
            ++index;
        }
        return std::nullopt;
    }

    // Only the shapes this schema emits occur -- strings, numbers, arrays and
    // objects -- so the scan tracks nesting depth and string state and stops at
    // the first top-level ',' or '}'.
    auto skipValue(
        std::string_view line,
        std::size_t      start
    ) noexcept -> std::optional<std::size_t>
    {
        auto depth = 0U;
        auto index = start;
        while (index < line.size())
        {
            auto const character = line[index];
            if (character == '"')
            {
                auto const next = skipString(line, index);
                if (!next)
                {
                    return std::nullopt;
                }
                index = *next;
                continue;
            }
            if (character == '{' || character == '[')
            {
                ++depth;
            }
            else if (character == '}' || character == ']')
            {
                if (depth == 0U)
                {
                    return index;
                }
                --depth;
            }
            else if (character == ',' && depth == 0U)
            {
                return index;
            }
            ++index;
        }
        return std::nullopt;
    }

    auto findTopLevelMember(
        std::string_view line,
        std::string_view name
    ) noexcept -> std::optional<MemberSpan>
    {
        if (line.size() < 2U || line.front() != '{')
        {
            return std::nullopt;
        }

        auto index = std::size_t{1};
        while (index < line.size() && line[index] != '}')
        {
            if (line[index] != '"')
            {
                return std::nullopt;
            }

            auto const keyEnd = skipString(line, index);
            if (!keyEnd || *keyEnd >= line.size() || line[*keyEnd] != ':')
            {
                return std::nullopt;
            }

            auto const valueEnd = skipValue(line, *keyEnd + 1U);
            if (!valueEnd)
            {
                return std::nullopt;
            }

            // The key is stored escaped; every name this schema emits is plain
            // ASCII, so comparing the quoted spelling is exact.
            auto const key = line.substr(index, *keyEnd - index);
            if (key == escapeJsonString(name))
            {
                return MemberSpan{.begin = index, .end = *valueEnd};
            }

            index = *valueEnd;
            if (index < line.size() && line[index] == ',')
            {
                ++index;
            }
        }

        return std::nullopt;
    }

    auto memberString(
        std::string_view line,
        std::string_view name
    ) -> std::optional<std::string>
    {
        auto const span = findTopLevelMember(line, name);
        if (!span)
        {
            return std::nullopt;
        }

        // Past the key and the colon: the value is what remains of the span, and
        // the span is well-formed because findTopLevelMember walked it.
        auto const keyEnd = skipString(line, span->begin);
        if (!keyEnd || *keyEnd >= span->end)
        {
            return std::nullopt;
        }
        auto const raw = line.substr(*keyEnd + 1U, span->end - *keyEnd - 1U);
        if (raw.size() < 2U || raw.front() != '"' || raw.back() != '"')
        {
            return std::nullopt;
        }

        auto decoded = std::string{};
        decoded.reserve(raw.size());
        for (auto index = std::size_t{1}; index + 1U < raw.size(); ++index)
        {
            if (raw[index] != '\\')
            {
                decoded.push_back(raw[index]);
                continue;
            }
            ++index;
            if (index + 1U >= raw.size())
            {
                return std::nullopt;
            }
            switch (raw[index])
            {
            case '"':  decoded.push_back('"');  break;
            case '\\': decoded.push_back('\\'); break;
            case '/':  decoded.push_back('/');  break;
            case 'b':  decoded.push_back('\b'); break;
            case 'f':  decoded.push_back('\f'); break;
            case 'n':  decoded.push_back('\n'); break;
            case 'r':  decoded.push_back('\r'); break;
            case 't':  decoded.push_back('\t'); break;
            case 'u':
            {
                // \u00XX and nothing wider. A surrogate pair or a codepoint above
                // 0xFF is a string this module cannot have written, and guessing
                // at one would put bytes in a page name that the model would then
                // be judged against.
                if (index + 4U >= raw.size())
                {
                    return std::nullopt;
                }
                if (raw[index + 1U] != '0' || raw[index + 2U] != '0')
                {
                    return std::nullopt;
                }
                auto const high = hexDigit(raw[index + 3U]);
                auto const low  = hexDigit(raw[index + 4U]);
                if (!high || !low)
                {
                    return std::nullopt;
                }
                decoded.push_back(
                    static_cast<char>(static_cast<uint8>(*high * 16U + *low))
                );
                index += 4U;
                break;
            }
            default:
                return std::nullopt;
            }
        }
        return decoded;
    }

    auto memberUnsigned(
        std::string_view line,
        std::string_view name
    ) noexcept -> std::optional<uint64>
    {
        auto const span = findTopLevelMember(line, name);
        if (!span)
        {
            return std::nullopt;
        }
        auto const keyEnd = skipString(line, span->begin);
        if (!keyEnd || *keyEnd >= span->end)
        {
            return std::nullopt;
        }
        auto const raw = line.substr(*keyEnd + 1U, span->end - *keyEnd - 1U);

        auto value = uint64{};
        auto const result =
            std::from_chars(raw.data(), raw.data() + raw.size(), value);
        if (result.ec != std::errc{} || result.ptr != raw.data() + raw.size())
        {
            return std::nullopt;
        }
        return value;
    }
}
