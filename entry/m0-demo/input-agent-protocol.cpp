#include "input-agent-protocol.hpp"

#include <core/numeric/checked-cast.hpp>
#include <domain/error.hpp>

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace
{
    constexpr auto g_maximumCommandBytes = std::size_t{64} * 1024U;

    [[nodiscard]]
    auto invalidCommand(std::string message) -> std::unexpected<uf::Error>
    {
        return uf::fail(
            uf::AutomationErrorKind::InvalidResource,
            std::move(message)
        );
    }

    [[nodiscard]]
    constexpr auto isDigit(char value) noexcept -> bool
    {
        return value >= '0' && value <= '9';
    }

    [[nodiscard]]
    constexpr auto isNonzeroDigit(char value) noexcept -> bool
    {
        return value >= '1' && value <= '9';
    }

    [[nodiscard]]
    auto isValidUtf8(std::string_view value) noexcept -> bool
    {
        auto codePoint = std::uint32_t{0};
        auto minimumCodePoint = std::uint32_t{0};
        auto continuationBytes = std::uint8_t{0};

        for (auto const character : value)
        {
            auto const byte = static_cast<std::uint32_t>(
                static_cast<unsigned char>(character)
            );
            if (continuationBytes == 0U)
            {
                if (byte <= 0x7FU)
                {
                    continue;
                }
                if (byte >= 0xC2U && byte <= 0xDFU)
                {
                    codePoint = byte & 0x1FU;
                    minimumCodePoint = 0x80U;
                    continuationBytes = 1U;
                    continue;
                }
                if (byte >= 0xE0U && byte <= 0xEFU)
                {
                    codePoint = byte & 0x0FU;
                    minimumCodePoint = 0x800U;
                    continuationBytes = 2U;
                    continue;
                }
                if (byte >= 0xF0U && byte <= 0xF4U)
                {
                    codePoint = byte & 0x07U;
                    minimumCodePoint = 0x10000U;
                    continuationBytes = 3U;
                    continue;
                }
                return false;
            }

            if ((byte & 0xC0U) != 0x80U)
            {
                return false;
            }
            codePoint = (codePoint << 6U) | (byte & 0x3FU);
            --continuationBytes;
            if (
                continuationBytes == 0U
                && (
                    codePoint < minimumCodePoint
                    || codePoint > 0x10FFFFU
                    || (codePoint >= 0xD800U && codePoint <= 0xDFFFU)
                )
            )
            {
                return false;
            }
        }

        return continuationBytes == 0U;
    }

    [[nodiscard]]
    auto hexValue(char value) noexcept -> std::optional<std::uint32_t>
    {
        if (value >= '0' && value <= '9')
        {
            return static_cast<std::uint32_t>(value - '0');
        }
        if (value >= 'a' && value <= 'f')
        {
            return static_cast<std::uint32_t>(value - 'a') + 10U;
        }
        if (value >= 'A' && value <= 'F')
        {
            return static_cast<std::uint32_t>(value - 'A') + 10U;
        }
        return std::nullopt;
    }

    auto appendUtf8(std::string& output, std::uint32_t codePoint) -> void
    {
        if (codePoint <= 0x7FU)
        {
            output += static_cast<char>(codePoint);
            return;
        }
        if (codePoint <= 0x7FFU)
        {
            output += static_cast<char>(0xC0U | (codePoint >> 6U));
            output += static_cast<char>(0x80U | (codePoint & 0x3FU));
            return;
        }
        if (codePoint <= 0xFFFFU)
        {
            output += static_cast<char>(0xE0U | (codePoint >> 12U));
            output += static_cast<char>(
                0x80U | ((codePoint >> 6U) & 0x3FU)
            );
            output += static_cast<char>(0x80U | (codePoint & 0x3FU));
            return;
        }

        output += static_cast<char>(0xF0U | (codePoint >> 18U));
        output += static_cast<char>(
            0x80U | ((codePoint >> 12U) & 0x3FU)
        );
        output += static_cast<char>(
            0x80U | ((codePoint >> 6U) & 0x3FU)
        );
        output += static_cast<char>(0x80U | (codePoint & 0x3FU));
    }

    struct ParsedCommandFields final
    {
        std::optional<std::string> m_operation;
        std::optional<std::string> m_output;
        std::optional<float> m_x;
        std::optional<float> m_y;
        std::optional<std::string> m_outputBefore;
        std::optional<std::string> m_outputAfter;
        std::optional<uf::MonotonicInstant::Duration> m_settle;
    };

    class CommandParser final
    {
        std::string m_source;
        std::size_t m_position{};

    public:
        explicit CommandParser(std::string_view source)
            : m_source{source}
        {
        }

    private:

        [[nodiscard]] auto atEnd() const noexcept -> bool
        {
            return m_position >= m_source.size();
        }

        [[nodiscard]] auto peek() const noexcept -> std::optional<char>
        {
            if (atEnd())
            {
                return std::nullopt;
            }
            return m_source[m_position];
        }

        [[nodiscard]]
        auto take(std::string_view expected) -> uf::Result<char>
        {
            if (atEnd())
            {
                return invalidCommand(
                    std::format(
                        "input-agent command ended while reading {}",
                        expected
                    )
                );
            }
            auto const value = m_source[m_position];
            ++m_position;
            return value;
        }

        auto skipWhitespace() noexcept -> void
        {
            while (auto const value = peek())
            {
                if (
                    *value != ' '
                    && *value != '\t'
                    && *value != '\r'
                    && *value != '\n'
                )
                {
                    break;
                }
                ++m_position;
            }
        }

        [[nodiscard]] auto consume(char expected) noexcept -> bool
        {
            if (peek() != expected)
            {
                return false;
            }
            ++m_position;
            return true;
        }

        [[nodiscard]]
        auto expect(char expected, std::string_view description) -> uf::Status
        {
            if (consume(expected))
            {
                return uf::ok();
            }
            return invalidCommand(
                std::format(
                    "input-agent command expected {} at byte {}",
                    description,
                    m_position
                )
            );
        }

        [[nodiscard]] auto parseHexCodeUnit() -> uf::Result<std::uint32_t>
        {
            auto codeUnit = std::uint32_t{};
            for (auto index = std::size_t{0}; index < 4U; ++index)
            {
                UF_TRY_VALUE(character, take("a Unicode escape"));
                auto const digit = hexValue(character);
                if (!digit)
                {
                    return invalidCommand(
                        std::format(
                            "input-agent command has an invalid Unicode escape at byte {}",
                            m_position - 1U
                        )
                    );
                }
                codeUnit = (codeUnit << 4U) | *digit;
            }
            return codeUnit;
        }

        [[nodiscard]] auto parseEscapedCodePoint() -> uf::Result<std::uint32_t>
        {
            UF_TRY_VALUE(first, parseHexCodeUnit());
            if (first >= 0xDC00U && first <= 0xDFFFU)
            {
                return invalidCommand(
                    "input-agent command contains a lone low Unicode surrogate"
                );
            }
            if (first < 0xD800U || first > 0xDBFFU)
            {
                return first;
            }

            UF_TRY(expect('\\', "a low-surrogate escape"));
            UF_TRY(expect('u', "a low-surrogate Unicode marker"));
            UF_TRY_VALUE(second, parseHexCodeUnit());
            if (second < 0xDC00U || second > 0xDFFFU)
            {
                return invalidCommand(
                    "input-agent command has an invalid Unicode surrogate pair"
                );
            }
            return (
                0x10000U
                + ((first - 0xD800U) << 10U)
                + (second - 0xDC00U)
            );
        }

        [[nodiscard]] auto parseString() -> uf::Result<std::string>
        {
            UF_TRY(expect('"', "a JSON string"));
            auto output = std::string{};
            while (true)
            {
                UF_TRY_VALUE(character, take("a JSON string"));
                if (character == '"')
                {
                    return output;
                }
                if (static_cast<unsigned char>(character) < 0x20U)
                {
                    return invalidCommand(
                        "input-agent command contains an unescaped control character"
                    );
                }
                if (character != '\\')
                {
                    output += character;
                    continue;
                }

                UF_TRY_VALUE(escaped, take("a JSON escape"));
                switch (escaped)
                {
                case '"': output += '"'; break;
                case '\\': output += '\\'; break;
                case '/': output += '/'; break;
                case 'b': output += '\b'; break;
                case 'f': output += '\f'; break;
                case 'n': output += '\n'; break;
                case 'r': output += '\r'; break;
                case 't': output += '\t'; break;
                case 'u':
                {
                    UF_TRY_VALUE(codePoint, parseEscapedCodePoint());
                    if (codePoint == 0U)
                    {
                        return invalidCommand(
                            "input-agent command strings must not contain null characters"
                        );
                    }
                    appendUtf8(output, codePoint);
                    break;
                }
                default:
                    return invalidCommand(
                        std::format(
                            "input-agent command has an invalid JSON escape at byte {}",
                            m_position - 1U
                        )
                    );
                }
            }
        }

        auto consumeDigits() noexcept -> void
        {
            while (auto const value = peek())
            {
                if (!isDigit(*value))
                {
                    break;
                }
                ++m_position;
            }
        }

        [[nodiscard]] auto parseNumberToken() -> uf::Result<std::string>
        {
            auto const start = m_position;
            static_cast<void>(consume('-'));

            auto const integerStart = peek();
            if (!integerStart)
            {
                return invalidCommand("input-agent command has an incomplete number");
            }
            if (*integerStart == '0')
            {
                ++m_position;
                if (auto const next = peek(); next && isDigit(*next))
                {
                    return invalidCommand(
                        "input-agent command number has a leading zero"
                    );
                }
            }
            else if (isNonzeroDigit(*integerStart))
            {
                consumeDigits();
            }
            else
            {
                return invalidCommand(
                    std::format(
                        "input-agent command expected a JSON number at byte {}",
                        m_position
                    )
                );
            }

            if (consume('.'))
            {
                auto const fractionStart = peek();
                if (!fractionStart || !isDigit(*fractionStart))
                {
                    return invalidCommand(
                        "input-agent command number has an empty fraction"
                    );
                }
                consumeDigits();
            }

            if (auto const exponent = peek(); exponent && (*exponent == 'e' || *exponent == 'E'))
            {
                ++m_position;
                if (auto const sign = peek(); sign && (*sign == '+' || *sign == '-'))
                {
                    ++m_position;
                }
                auto const exponentStart = peek();
                if (!exponentStart || !isDigit(*exponentStart))
                {
                    return invalidCommand(
                        "input-agent command number has an empty exponent"
                    );
                }
                consumeDigits();
            }

            return std::string{m_source.substr(start, m_position - start)};
        }

        [[nodiscard]] auto parseCoordinate(std::string_view field) -> uf::Result<float>
        {
            UF_TRY_VALUE(token, parseNumberToken());
            auto coordinate = 0.0F;
            auto const* const begin = token.data();
            auto const* const end = begin + token.size();
            auto const parsed = std::from_chars(
                begin,
                end,
                coordinate,
                std::chars_format::general
            );
            if (
                parsed.ec != std::errc{}
                || parsed.ptr != end
                || !std::isfinite(coordinate)
            )
            {
                return invalidCommand(
                    std::format(
                        "input-agent command field {} is not a finite coordinate",
                        field
                    )
                );
            }
            return coordinate;
        }

        [[nodiscard]]
        auto parseSettle() -> uf::Result<uf::MonotonicInstant::Duration>
        {
            UF_TRY_VALUE(token, parseNumberToken());
            auto milliseconds = std::uint64_t{};
            auto const* const begin = token.data();
            auto const* const end = begin + token.size();
            auto const parsed = std::from_chars(
                begin,
                end,
                milliseconds,
                10
            );
            if (parsed.ec != std::errc{} || parsed.ptr != end)
            {
                return invalidCommand(
                    "input-agent command field settle_ms must be a non-negative integer"
                );
            }

            using Milliseconds = std::chrono::milliseconds;
            using Duration = uf::MonotonicInstant::Duration;
            auto const maximum = std::chrono::duration_cast<Milliseconds>(
                uf::m0_demo::g_maximumInputAgentSettle
            );
            auto const maximumCount = uf::checkedCast<std::uint64_t>(maximum.count());
            auto const count = uf::checkedCast<Milliseconds::rep>(milliseconds);
            if (
                !maximumCount
                || milliseconds > *maximumCount
                || !count
            )
            {
                return invalidCommand(
                    std::format(
                        "input-agent command field settle_ms must not exceed {}",
                        maximum.count()
                    )
                );
            }
            return std::chrono::duration_cast<Duration>(Milliseconds{*count});
        }

        template <typename Value>
        [[nodiscard]]
        auto setOnce(
            std::optional<Value>& destination,
            Value value,
            std::string_view field
        ) -> uf::Status
        {
            if (destination)
            {
                return invalidCommand(
                    std::format(
                        "input-agent command repeats field {}",
                        field
                    )
                );
            }
            destination = std::move(value);
            return uf::ok();
        }

        [[nodiscard]]
        auto parseField(
            ParsedCommandFields& fields,
            std::string const& field
        ) -> uf::Status
        {
            if (field == "op")
            {
                UF_TRY_VALUE(value, parseString());
                return setOnce(fields.m_operation, std::move(value), field);
            }
            if (field == "out")
            {
                UF_TRY_VALUE(value, parseString());
                return setOnce(fields.m_output, std::move(value), field);
            }
            if (field == "x")
            {
                UF_TRY_VALUE(value, parseCoordinate(field));
                return setOnce(fields.m_x, value, field);
            }
            if (field == "y")
            {
                UF_TRY_VALUE(value, parseCoordinate(field));
                return setOnce(fields.m_y, value, field);
            }
            if (field == "out_before")
            {
                UF_TRY_VALUE(value, parseString());
                return setOnce(fields.m_outputBefore, std::move(value), field);
            }
            if (field == "out_after")
            {
                UF_TRY_VALUE(value, parseString());
                return setOnce(fields.m_outputAfter, std::move(value), field);
            }
            if (field == "settle_ms")
            {
                UF_TRY_VALUE(value, parseSettle());
                return setOnce(fields.m_settle, value, field);
            }
            return invalidCommand(
                std::format(
                    "input-agent command has unrecognized field \"{}\"",
                    field
                )
            );
        }

    public:
        [[nodiscard]] auto parseFields() -> uf::Result<ParsedCommandFields>
        {
            auto fields = ParsedCommandFields{};
            skipWhitespace();
            UF_TRY(expect('{', "a JSON object"));
            skipWhitespace();
            if (consume('}'))
            {
                skipWhitespace();
                if (!atEnd())
                {
                    return invalidCommand(
                        "input-agent command has trailing content"
                    );
                }
                return fields;
            }

            while (true)
            {
                UF_TRY_VALUE(field, parseString());
                skipWhitespace();
                UF_TRY(expect(':', "a field-value separator"));
                skipWhitespace();
                UF_TRY(parseField(fields, field));
                skipWhitespace();
                if (consume('}'))
                {
                    break;
                }
                UF_TRY(expect(',', "a field separator"));
                skipWhitespace();
            }

            skipWhitespace();
            if (!atEnd())
            {
                return invalidCommand(
                    "input-agent command has trailing content"
                );
            }
            return fields;
        }
    };

    [[nodiscard]]
    auto requirePath(
        std::optional<std::string> value,
        std::string_view field
    ) -> uf::Result<std::filesystem::path>
    {
        if (!value || value->empty())
        {
            return invalidCommand(
                std::format(
                    "input-agent command requires a non-empty {} field",
                    field
                )
            );
        }
        if (value->contains('\0'))
        {
            return invalidCommand(
                std::format(
                    "input-agent command field {} contains a null character",
                    field
                )
            );
        }
        try
        {
            return std::filesystem::path{*std::move(value)};
        }
        catch (std::filesystem::filesystem_error const& error)
        {
            return invalidCommand(
                std::format(
                    "input-agent command field {} is not a valid path: {}",
                    field,
                    error.what()
                )
            );
        }
    }

    [[nodiscard]]
    auto commandFromFields(
        ParsedCommandFields fields
    ) -> uf::Result<uf::m0_demo::InputAgentCommand>
    {
        if (!fields.m_operation)
        {
            return invalidCommand("input-agent command is missing field op");
        }

        if (*fields.m_operation == "capture")
        {
            if (
                fields.m_x
                || fields.m_y
                || fields.m_outputBefore
                || fields.m_outputAfter
                || fields.m_settle
            )
            {
                return invalidCommand(
                    "input-agent capture command contains click-only fields"
                );
            }
            UF_TRY_VALUE(output, requirePath(std::move(fields.m_output), "out"));
            return uf::m0_demo::InputAgentCaptureCommand{
                .m_output = std::move(output),
            };
        }

        if (*fields.m_operation == "click")
        {
            if (fields.m_output)
            {
                return invalidCommand(
                    "input-agent click command contains capture-only field out"
                );
            }
            if (!fields.m_x || !fields.m_y)
            {
                return invalidCommand(
                    "input-agent click command requires x and y fields"
                );
            }
            UF_TRY_VALUE(
                outputBefore,
                requirePath(std::move(fields.m_outputBefore), "out_before")
            );
            UF_TRY_VALUE(
                outputAfter,
                requirePath(std::move(fields.m_outputAfter), "out_after")
            );
            return uf::m0_demo::InputAgentClickCommand{
                .m_x = *fields.m_x,
                .m_y = *fields.m_y,
                .m_outputBefore = std::move(outputBefore),
                .m_outputAfter = std::move(outputAfter),
                .m_settle = fields.m_settle.value_or(
                    uf::m0_demo::g_defaultInputAgentSettle
                ),
            };
        }

        if (*fields.m_operation == "quit")
        {
            if (
                fields.m_output
                || fields.m_x
                || fields.m_y
                || fields.m_outputBefore
                || fields.m_outputAfter
                || fields.m_settle
            )
            {
                return invalidCommand(
                    "input-agent quit command must contain only field op"
                );
            }
            return uf::m0_demo::InputAgentQuitCommand{};
        }

        return invalidCommand(
            std::format(
                "input-agent command has unrecognized op \"{}\"",
                *fields.m_operation
            )
        );
    }
}

namespace uf::m0_demo
{
    auto parseInputAgentCommand(
        std::string_view line
    ) -> Result<InputAgentCommand>
    {
        if (line.size() > g_maximumCommandBytes)
        {
            return invalidCommand(
                std::format(
                    "input-agent command exceeds the {}-byte limit",
                    g_maximumCommandBytes
                )
            );
        }
        if (!isValidUtf8(line))
        {
            return invalidCommand(
                "input-agent command must contain valid UTF-8"
            );
        }
        auto parser = CommandParser{line};
        UF_TRY_VALUE(fields, parser.parseFields());
        return commandFromFields(std::move(fields));
    }
}
