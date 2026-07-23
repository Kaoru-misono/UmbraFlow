#include "canonical-toml.hpp"

#include <core/numeric/checked-cast.hpp>
#include <core/safety/checked-access.hpp>
#include <core/text/utf8.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <charconv>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::annotation::detail
{
    namespace
    {
        constexpr auto g_hexDigits = std::string_view{"0123456789abcdef"};

        [[nodiscard]]
        constexpr auto lowerHexValue(char value) noexcept -> std::optional<uint8>
        {
            if (value >= '0' && value <= '9')
            {
                return static_cast<uint8>(value - '0');
            }
            if (value >= 'a' && value <= 'f')
            {
                return static_cast<uint8>(value - 'a' + 10);
            }
            return std::nullopt;
        }
    }

    auto CanonicalTomlReader::invalid(
        std::string message
    ) const -> std::unexpected<Error>
    {
        return fail(
            AutomationErrorKind::InvalidResource,
            std::format("{} {}", m_documentName, std::move(message))
        );
    }

    auto CanonicalTomlReader::lineAtOffset() const -> Result<std::string>
    {
        if (m_offset >= m_text.size())
        {
            return invalid(
                std::format("ended unexpectedly at line {}", m_line)
            );
        }

        auto const end = m_text.find('\n', m_offset);
        if (end == std::string::npos)
        {
            return invalid(
                std::format(
                    "line {} is missing its canonical LF terminator",
                    m_line
                )
            );
        }
        auto const line = m_text.substr(m_offset, end - m_offset);
        if (line.contains('\r'))
        {
            return invalid(
                std::format(
                    "line {} contains a non-canonical carriage return",
                    m_line
                )
            );
        }
        return line;
    }

    auto CanonicalTomlReader::fieldValue(
        std::string_view line,
        std::string_view key
    ) const -> Result<std::string>
    {
        auto prefix = std::string{key};
        prefix += " = ";
        if (!line.starts_with(prefix))
        {
            return invalid(std::format("expected field '{}'", key));
        }
        return std::string{line.substr(prefix.size())};
    }

    auto CanonicalTomlReader::parseTomlString(
        std::string_view encoded
    ) const -> Result<std::string>
    {
        if (
            encoded.size() < 2U
            || encoded.front() != '"'
            || encoded.back() != '"'
        )
        {
            return invalid("string must use one-line basic-string syntax");
        }

        auto result = std::string{};
        result.reserve(encoded.size() - 2U);
        for (auto index = std::size_t{1}; index + 1U < encoded.size(); ++index)
        {
            auto const character = checkedAt(encoded, index);
            auto const byte = static_cast<uint8>(
                static_cast<unsigned char>(character)
            );
            if (character == '"' || byte < uint8{0x20} || byte == uint8{0x7F})
            {
                return invalid("string contains an unescaped character");
            }
            if (character != '\\')
            {
                result.push_back(character);
                continue;
            }

            ++index;
            if (index + 1U >= encoded.size())
            {
                return invalid("string ends inside an escape sequence");
            }
            auto const escaped = checkedAt(encoded, index);
            switch (escaped)
            {
            case '"':
                result.push_back('"');
                break;
            case '\\':
                result.push_back('\\');
                break;
            case 'b':
                result.push_back('\b');
                break;
            case 't':
                result.push_back('\t');
                break;
            case 'n':
                result.push_back('\n');
                break;
            case 'f':
                result.push_back('\f');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 'u':
            {
                if (index + 4U >= encoded.size() - 1U)
                {
                    return invalid("string has a truncated Unicode escape");
                }
                auto const first  = lowerHexValue(checkedAt(encoded, index + 1U));
                auto const second = lowerHexValue(checkedAt(encoded, index + 2U));
                auto const third  = lowerHexValue(checkedAt(encoded, index + 3U));
                auto const fourth = lowerHexValue(checkedAt(encoded, index + 4U));
                if (
                    !first
                    || !second
                    || !third
                    || !fourth
                    || *first != 0U
                    || *second != 0U
                )
                {
                    return invalid(
                        "accepts only canonical \\u00xx control escapes"
                    );
                }
                auto const scalar = static_cast<uint8>((*third << 4U) | *fourth);
                result.push_back(static_cast<char>(scalar));
                index += 4U;
                break;
            }
            default:
                return invalid("string contains an unsupported escape");
            }
        }

        if (!isValidUtf8(result))
        {
            return invalid("string is not valid UTF-8");
        }
        return result;
    }

    auto CanonicalTomlReader::parseUnsigned64(
        std::string_view encoded
    ) const -> Result<uint64>
    {
        if (encoded.empty())
        {
            return invalid("integer must contain a decimal digit");
        }

        auto value              = uint64{0};
        auto const* const first = std::to_address(encoded.begin());
        auto const* const last  = std::to_address(encoded.end());
        auto const [end, error] = std::from_chars(first, last, value, 10);
        if (error == std::errc::result_out_of_range)
        {
            return invalid("integer exceeds uint64");
        }
        if (error != std::errc{} || end != last)
        {
            return invalid("integer must be unsigned decimal");
        }
        return value;
    }

    auto CanonicalTomlReader::parseUnsigned32Array(
        std::string_view encoded
    ) const -> Result<std::vector<uint32>>
    {
        if (
            encoded.size() < 2U
            || encoded.front() != '['
            || encoded.back() != ']'
        )
        {
            return invalid("integer array must use bracket syntax");
        }

        auto values     = std::vector<uint32>{};
        auto const end  = encoded.size() - 1U;
        auto position   = std::size_t{1};
        if (position == end)
        {
            return values;
        }
        while (position < end)
        {
            auto const separator = encoded.find(", ", position);
            auto const itemEnd = separator == std::string_view::npos
                ? end
                : separator;
            if (itemEnd > end)
            {
                return invalid("integer array has invalid separators");
            }
            UF_TRY_VALUE(
                wideValue,
                parseUnsigned64(encoded.substr(position, itemEnd - position))
            );
            auto const value = checkedCast<uint32>(wideValue);
            if (!value)
            {
                return invalid("integer array item exceeds uint32");
            }
            values.emplace_back(*value);
            if (separator == std::string_view::npos)
            {
                position = end;
            }
            else
            {
                position = separator + 2U;
            }
        }
        return values;
    }

    auto CanonicalTomlReader::parseStringArray(
        std::string_view encoded
    ) const -> Result<std::vector<std::string>>
    {
        if (
            encoded.size() < 2U
            || encoded.front() != '['
            || encoded.back() != ']'
        )
        {
            return invalid("string array must use bracket syntax");
        }

        auto values     = std::vector<std::string>{};
        auto const end  = encoded.size() - 1U;
        auto position   = std::size_t{1};
        if (position == end)
        {
            return values;
        }
        while (position < end)
        {
            if (checkedAt(encoded, position) != '"')
            {
                return invalid("string array item must be quoted");
            }

            auto itemEnd = position + 1U;
            auto escaped = false;
            for (; itemEnd < end; ++itemEnd)
            {
                auto const character = checkedAt(encoded, itemEnd);
                if (!escaped && character == '"')
                {
                    break;
                }
                if (!escaped && character == '\\')
                {
                    escaped = true;
                }
                else
                {
                    escaped = false;
                }
            }
            if (itemEnd >= end || checkedAt(encoded, itemEnd) != '"')
            {
                return invalid("string array has an unterminated item");
            }
            UF_TRY_VALUE(
                value,
                parseTomlString(
                    encoded.substr(position, itemEnd - position + 1U)
                )
            );
            values.emplace_back(std::move(value));
            position = itemEnd + 1U;
            if (position == end)
            {
                break;
            }
            if (
                end - position < 2U
                || encoded.substr(position, 2U) != ", "
            )
            {
                return invalid("string array has invalid separators");
            }
            position += 2U;
        }
        return values;
    }

    CanonicalTomlReader::CanonicalTomlReader(
        std::string documentName,
        std::string text
    ) noexcept
        : m_documentName{std::move(documentName)}
        , m_text{std::move(text)}
    {
    }

    auto CanonicalTomlReader::eof() const noexcept -> bool
    {
        return m_offset == m_text.size();
    }

    auto CanonicalTomlReader::line() const noexcept -> std::size_t
    {
        return m_line;
    }

    auto CanonicalTomlReader::take() -> Result<std::string>
    {
        UF_TRY_VALUE(current, lineAtOffset());
        m_offset += current.size() + 1U;
        ++m_line;
        return current;
    }

    auto CanonicalTomlReader::expect(std::string_view expected) -> Status
    {
        auto const expectedLine = m_line;
        UF_TRY_VALUE(actual, take());
        if (actual != expected)
        {
            return invalid(
                std::format(
                    "line {} expected '{}', found '{}'",
                    expectedLine,
                    expected,
                    actual
                )
            );
        }
        return ok();
    }

    auto CanonicalTomlReader::takeStringField(
        std::string_view key
    ) -> Result<std::string>
    {
        UF_TRY_VALUE(lineText, take());
        UF_TRY_VALUE(value, fieldValue(lineText, key));
        return parseTomlString(value);
    }

    auto CanonicalTomlReader::takeUnsigned32Field(
        std::string_view key
    ) -> Result<uint32>
    {
        UF_TRY_VALUE(value, takeUnsigned64Field(key));
        auto const narrowed = checkedCast<uint32>(value);
        if (!narrowed)
        {
            return invalid(std::format("field '{}' exceeds uint32", key));
        }
        return *narrowed;
    }

    auto CanonicalTomlReader::takeUnsigned64Field(
        std::string_view key
    ) -> Result<uint64>
    {
        UF_TRY_VALUE(lineText, take());
        UF_TRY_VALUE(value, fieldValue(lineText, key));
        return parseUnsigned64(value);
    }

    auto CanonicalTomlReader::takeUnsigned32ArrayField(
        std::string_view key
    ) -> Result<std::vector<uint32>>
    {
        UF_TRY_VALUE(lineText, take());
        UF_TRY_VALUE(value, fieldValue(lineText, key));
        return parseUnsigned32Array(value);
    }

    auto CanonicalTomlReader::takeStringArrayField(
        std::string_view key
    ) -> Result<std::vector<std::string>>
    {
        UF_TRY_VALUE(lineText, take());
        UF_TRY_VALUE(value, fieldValue(lineText, key));
        return parseStringArray(value);
    }

    auto CanonicalTomlReader::nextIsField(
        std::string_view key
    ) const -> Result<bool>
    {
        if (eof())
        {
            return false;
        }
        UF_TRY_VALUE(lineText, lineAtOffset());
        auto prefix = std::string{key};
        prefix += " = ";
        return lineText.starts_with(prefix);
    }

    auto appendTomlString(
        std::string& output,
        std::string_view value
    ) -> void
    {
        output.push_back('"');
        for (auto const character : value)
        {
            auto const byte = static_cast<uint8>(
                static_cast<unsigned char>(character)
            );
            switch (character)
            {
            case '"':
                output += "\\\"";
                break;
            case '\\':
                output += "\\\\";
                break;
            case '\b':
                output += "\\b";
                break;
            case '\t':
                output += "\\t";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\f':
                output += "\\f";
                break;
            case '\r':
                output += "\\r";
                break;
            default:
                if (byte < uint8{0x20} || byte == uint8{0x7F})
                {
                    output += "\\u00";
                    output.push_back(checkedAt(g_hexDigits, byte >> 4U));
                    output.push_back(
                        checkedAt(g_hexDigits, byte & uint8{0x0F})
                    );
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

    auto appendStringField(
        std::string& output,
        std::string_view key,
        std::string_view value
    ) -> void
    {
        output += key;
        output += " = ";
        appendTomlString(output, value);
        output.push_back('\n');
    }

    auto appendUnsigned32Array(
        std::string& output,
        std::span<uint32 const> values
    ) -> void
    {
        output.push_back('[');
        for (auto index = std::size_t{0}; index < values.size(); ++index)
        {
            if (index != 0U)
            {
                output += ", ";
            }
            output += std::to_string(checkedAt(values, index));
        }
        output.push_back(']');
    }
}
