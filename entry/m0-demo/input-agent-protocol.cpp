#include "input-agent-protocol.hpp"

#include <core/numeric/checked-cast.hpp>
#include <core/text/utf8.hpp>
#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace uf::m0_demo
{
    namespace
    {
        constexpr auto k_maximumCommandBytes = std::size_t{64} * 1024U;

        [[nodiscard]]
        auto invalidCommand(std::string message) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
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
        auto hexValue(char value) noexcept -> std::optional<uint32>
        {
            if (value >= '0' && value <= '9')
            {
                return checkedCast<uint32>(value - '0');
            }
            if (value >= 'a' && value <= 'f')
            {
                return checkedCast<uint32>(value - 'a' + 10);
            }
            if (value >= 'A' && value <= 'F')
            {
                return checkedCast<uint32>(value - 'A' + 10);
            }
            return std::nullopt;
        }

        struct ParsedCommandFields final
        {
            using Duration = MonotonicInstant::Duration;
            std::optional<std::string> operation{};
            std::optional<std::string> output{};
            std::optional<std::string> key{};
            std::optional<float>       x{};
            std::optional<float>       y{};
            std::optional<int32>       delta{};
            std::optional<std::string> outputBefore{};
            std::optional<std::string> outputAfter{};
            std::optional<Duration>    settle{};
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
            auto take(std::string_view expected) -> Result<char>
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
            auto expect(char expected, std::string_view description) -> Status
            {
                if (consume(expected))
                {
                    return ok();
                }
                return invalidCommand(
                    std::format(
                        "input-agent command expected {} at byte {}",
                        description,
                        m_position
                    )
                );
            }

            [[nodiscard]] auto parseHexCodeUnit() -> Result<uint32>
            {
                auto codeUnit = uint32{};
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

            [[nodiscard]] auto parseEscapedCodePoint() -> Result<uint32>
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

            [[nodiscard]] auto parseString() -> Result<std::string>
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
                    if (character >= '\0' && character < ' ')
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
                        appendUtf8Scalar(
                            output,
                            codePoint
                        );
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

            [[nodiscard]] auto parseNumberToken() -> Result<std::string>
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

            [[nodiscard]] auto parseCoordinate(std::string_view field) -> Result<float>
            {
                UF_TRY_VALUE(token, parseNumberToken());
                auto coordinate = 0.0F;
                auto const* const begin = std::to_address(token.cbegin());
                auto const* const end = std::to_address(token.cend());
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

            // Whole wheel notches, so a fraction or an exponent is refused here
            // rather than silently truncated. WheelDelta decides which counts are
            // deliverable.
            [[nodiscard]] auto parseWheelDelta() -> Result<int32>
            {
                UF_TRY_VALUE(token, parseNumberToken());
                auto notches = int32{};
                auto const* const begin = std::to_address(token.cbegin());
                auto const* const end = std::to_address(token.cend());
                auto const parsed = std::from_chars(
                    begin,
                    end,
                    notches,
                    10
                );
                if (parsed.ec != std::errc{} || parsed.ptr != end)
                {
                    return invalidCommand(
                        "input-agent command field delta must be a whole number of wheel notches"
                    );
                }
                return notches;
            }

            [[nodiscard]]
            auto parseSettle() -> Result<MonotonicInstant::Duration>
            {
                UF_TRY_VALUE(token, parseNumberToken());
                auto milliseconds = uint64{};
                auto const* const begin = std::to_address(token.cbegin());
                auto const* const end = std::to_address(token.cend());
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
                using Duration = MonotonicInstant::Duration;
                auto const maximum = std::chrono::duration_cast<Milliseconds>(
                    k_maximumInputAgentSettle
                );
                auto const maximumCount = checkedCast<uint64>(maximum.count());
                auto const count = checkedCast<Milliseconds::rep>(milliseconds);
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
            ) -> Status
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
                return ok();
            }

            [[nodiscard]]
            auto parseField(
                ParsedCommandFields& fields,
                std::string const& field
            ) -> Status
            {
                if (field == "op")
                {
                    UF_TRY_VALUE(value, parseString());
                    return setOnce(fields.operation, std::move(value), field);
                }
                if (field == "out")
                {
                    UF_TRY_VALUE(value, parseString());
                    return setOnce(fields.output, std::move(value), field);
                }
                if (field == "key")
                {
                    UF_TRY_VALUE(value, parseString());
                    return setOnce(fields.key, std::move(value), field);
                }
                if (field == "x")
                {
                    UF_TRY_VALUE(value, parseCoordinate(field));
                    return setOnce(fields.x, value, field);
                }
                if (field == "y")
                {
                    UF_TRY_VALUE(value, parseCoordinate(field));
                    return setOnce(fields.y, value, field);
                }
                if (field == "delta")
                {
                    UF_TRY_VALUE(value, parseWheelDelta());
                    return setOnce(fields.delta, value, field);
                }
                if (field == "out_before")
                {
                    UF_TRY_VALUE(value, parseString());
                    return setOnce(fields.outputBefore, std::move(value), field);
                }
                if (field == "out_after")
                {
                    UF_TRY_VALUE(value, parseString());
                    return setOnce(fields.outputAfter, std::move(value), field);
                }
                if (field == "settle_ms")
                {
                    UF_TRY_VALUE(value, parseSettle());
                    return setOnce(fields.settle, value, field);
                }
                return invalidCommand(
                    std::format(
                        "input-agent command has unrecognized field \"{}\"",
                        field
                    )
                );
            }

        public:
            [[nodiscard]] auto parseFields() -> Result<ParsedCommandFields>
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
        ) -> Result<std::filesystem::path>
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
        ) -> Result<InputAgentCommand>
        {
            if (!fields.operation)
            {
                return invalidCommand("input-agent command is missing field op");
            }

            if (*fields.operation == "capture")
            {
                if (
                    fields.key
                    || fields.x
                    || fields.y
                    || fields.delta
                    || fields.outputBefore
                    || fields.outputAfter
                    || fields.settle
                )
                {
                    return invalidCommand(
                        "input-agent capture command contains action-only fields"
                    );
                }
                UF_TRY_VALUE(output, requirePath(std::move(fields.output), "out"));
                return InputAgentCaptureCommand{
                    .output = std::move(output),
                };
            }

            if (*fields.operation == "click")
            {
                if (fields.output)
                {
                    return invalidCommand(
                        "input-agent click command contains capture-only field out"
                    );
                }
                if (fields.key)
                {
                    return invalidCommand(
                        "input-agent click command contains key-only field key"
                    );
                }
                if (fields.delta)
                {
                    return invalidCommand(
                        "input-agent click command contains scroll-only field delta"
                    );
                }
                if (!fields.x || !fields.y)
                {
                    return invalidCommand(
                        "input-agent click command requires x and y fields"
                    );
                }
                UF_TRY_VALUE(
                    outputBefore,
                    requirePath(std::move(fields.outputBefore), "out_before")
                );
                UF_TRY_VALUE(
                    outputAfter,
                    requirePath(std::move(fields.outputAfter), "out_after")
                );
                return InputAgentClickCommand{
                    .x            = *fields.x,
                    .y            = *fields.y,
                    .outputBefore = std::move(outputBefore),
                    .outputAfter  = std::move(outputAfter),
                    .settle = fields.settle.value_or(
                        k_defaultInputAgentSettle
                    ),
                };
            }

            if (*fields.operation == "key")
            {
                if (fields.output)
                {
                    return invalidCommand(
                        "input-agent key command contains capture-only field out"
                    );
                }
                if (fields.x || fields.y)
                {
                    return invalidCommand(
                        "input-agent key command contains click-only coordinate fields"
                    );
                }
                if (fields.delta)
                {
                    return invalidCommand(
                        "input-agent key command contains scroll-only field delta"
                    );
                }
                if (!fields.key)
                {
                    return invalidCommand(
                        "input-agent key command requires a key field"
                    );
                }
                auto const key = KeyInput::fromName(*fields.key);
                if (!key)
                {
                    return invalidCommand(
                        std::format(
                            "input-agent key command has an unsupported key: {}",
                            key.error().message()
                        )
                    );
                }
                UF_TRY_VALUE(
                    outputBefore,
                    requirePath(std::move(fields.outputBefore), "out_before")
                );
                UF_TRY_VALUE(
                    outputAfter,
                    requirePath(std::move(fields.outputAfter), "out_after")
                );
                return InputAgentKeyCommand{
                    .key          = *key,
                    .outputBefore = std::move(outputBefore),
                    .outputAfter  = std::move(outputAfter),
                    .settle = fields.settle.value_or(
                        k_defaultInputAgentSettle
                    ),
                };
            }

            if (*fields.operation == "scroll")
            {
                if (fields.output)
                {
                    return invalidCommand(
                        "input-agent scroll command contains capture-only field out"
                    );
                }
                if (fields.key)
                {
                    return invalidCommand(
                        "input-agent scroll command contains key-only field key"
                    );
                }
                if (!fields.x || !fields.y)
                {
                    return invalidCommand(
                        "input-agent scroll command requires x and y fields"
                    );
                }
                if (!fields.delta)
                {
                    return invalidCommand(
                        "input-agent scroll command requires a delta field"
                    );
                }
                auto const delta = WheelDelta::create(*fields.delta);
                if (!delta)
                {
                    return invalidCommand(
                        std::format(
                            "input-agent scroll command has an undeliverable delta: {}",
                            delta.error().message()
                        )
                    );
                }
                UF_TRY_VALUE(
                    outputBefore,
                    requirePath(std::move(fields.outputBefore), "out_before")
                );
                UF_TRY_VALUE(
                    outputAfter,
                    requirePath(std::move(fields.outputAfter), "out_after")
                );
                return InputAgentScrollCommand{
                    .x            = *fields.x,
                    .y            = *fields.y,
                    .delta        = *delta,
                    .outputBefore = std::move(outputBefore),
                    .outputAfter  = std::move(outputAfter),
                    .settle = fields.settle.value_or(
                        k_defaultInputAgentSettle
                    ),
                };
            }

            if (*fields.operation == "quit")
            {
                if (
                    fields.output
                    || fields.key
                    || fields.x
                    || fields.y
                    || fields.delta
                    || fields.outputBefore
                    || fields.outputAfter
                    || fields.settle
                )
                {
                    return invalidCommand(
                        "input-agent quit command must contain only field op"
                    );
                }
                return InputAgentQuitCommand{};
            }

            return invalidCommand(
                std::format(
                    "input-agent command has unrecognized op \"{}\"",
                    *fields.operation
                )
            );
        }
    }

    auto parseInputAgentCommand(
        std::string_view line
    ) -> Result<InputAgentCommand>
    {
        if (line.size() > k_maximumCommandBytes)
        {
            return invalidCommand(
                std::format(
                    "input-agent command exceeds the {}-byte limit",
                    k_maximumCommandBytes
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
