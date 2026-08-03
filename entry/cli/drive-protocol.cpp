#include "drive-protocol.hpp"

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/text/utf8.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/key.hpp>

#include <trace/json-text.hpp>

#include <charconv>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

namespace uf::cli
{
    namespace
    {
        [[nodiscard]]
        auto invalidCommand(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        [[nodiscard]]
        constexpr auto isDigit(char value) noexcept -> bool
        {
            return value >= '0' && value <= '9';
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

        // Every field the protocol knows, read once each. An absent optional is what
        // makes "a convenience command omitted a required policy field" a fact the
        // command builder can check rather than a value it has to guess at.
        struct ParsedFields final
        {
            std::optional<std::string> operation{};
            std::optional<std::string> key{};
            std::optional<uint64>      cycle{};
            std::optional<uint64>      deadline{};
            std::optional<uint64>      millis{};
            std::optional<uint64>      pollMillis{};
        };

        // A strict reader for the one JSON shape this protocol uses: a flat object of
        // strings and non-negative whole numbers. An unrecognized field, a repeated
        // field, a fractional number, a leading zero, an unescaped control byte and
        // trailing content are all REFUSED, because an operator command posts input
        // to a live target and a line that is nearly right must fail loudly. Floating
        // point is absent because no field here is fractional -- every one is a
        // count, an id or a name.
        class CommandReader final
        {
            std::string m_source;
            std::size_t m_position{};

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

            [[nodiscard]] auto take(std::string_view expected) -> Result<char>
            {
                if (atEnd())
                {
                    return invalidCommand(
                        std::format("command ended while reading {}", expected)
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
                        "command expected {} at byte {}",
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
                                "command has an invalid Unicode escape at byte {}",
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
                        "command contains a lone low Unicode surrogate"
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
                        "command has an invalid Unicode surrogate pair"
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
                            "command contains an unescaped control character"
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
                                "command strings must not contain null characters"
                            );
                        }
                        appendUtf8Scalar(output, codePoint);
                        break;
                    }
                    default:
                        return invalidCommand(
                            std::format(
                                "command has an invalid JSON escape at byte {}",
                                m_position - 1U
                            )
                        );
                    }
                }
            }

            [[nodiscard]]
            auto parseWholeNumber(std::string_view field) -> Result<uint64>
            {
                auto const start = m_position;
                auto const first = peek();
                if (!first || !isDigit(*first))
                {
                    return invalidCommand(
                        std::format(
                            "command field {} must be a non-negative whole number",
                            field
                        )
                    );
                }
                if (*first == '0')
                {
                    ++m_position;
                    if (auto const next = peek(); next && isDigit(*next))
                    {
                        return invalidCommand(
                            std::format(
                                "command field {} has a leading zero",
                                field
                            )
                        );
                    }
                }
                else
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
                if (auto const next = peek();
                    next && (*next == '.' || *next == 'e' || *next == 'E'))
                {
                    return invalidCommand(
                        std::format(
                            "command field {} must be whole, not fractional",
                            field
                        )
                    );
                }

                auto const token        = m_source.substr(start, m_position - start);
                auto       value        = uint64{};
                auto const* const begin = std::to_address(token.cbegin());
                auto const* const end   = std::to_address(token.cend());
                auto const parsed       = std::from_chars(begin, end, value, 10);
                if (parsed.ec != std::errc{} || parsed.ptr != end)
                {
                    return invalidCommand(
                        std::format("command field {} is out of range", field)
                    );
                }
                return value;
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
                        std::format("command repeats field {}", field)
                    );
                }
                destination = std::move(value);
                return ok();
            }

            [[nodiscard]]
            auto parseField(ParsedFields& fields, std::string const& field) -> Status
            {
                if (field == "op")
                {
                    UF_TRY_VALUE(value, parseString());
                    return setOnce(fields.operation, std::move(value), field);
                }
                if (field == "key")
                {
                    UF_TRY_VALUE(value, parseString());
                    return setOnce(fields.key, std::move(value), field);
                }
                if (field == "cycle")
                {
                    UF_TRY_VALUE(value, parseWholeNumber(field));
                    return setOnce(fields.cycle, value, field);
                }
                if (field == "deadline")
                {
                    UF_TRY_VALUE(value, parseWholeNumber(field));
                    return setOnce(fields.deadline, value, field);
                }
                if (field == "ms")
                {
                    UF_TRY_VALUE(value, parseWholeNumber(field));
                    return setOnce(fields.millis, value, field);
                }
                if (field == "poll_ms")
                {
                    UF_TRY_VALUE(value, parseWholeNumber(field));
                    return setOnce(fields.pollMillis, value, field);
                }
                return invalidCommand(
                    std::format("command has unrecognized field \"{}\"", field)
                );
            }

        public:
            explicit CommandReader(std::string_view source)
                : m_source{source}
            {
            }

            [[nodiscard]] auto parseFields() -> Result<ParsedFields>
            {
                auto fields = ParsedFields{};
                skipWhitespace();
                UF_TRY(expect('{', "a JSON object"));
                skipWhitespace();
                if (!consume('}'))
                {
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
                }

                skipWhitespace();
                if (!atEnd())
                {
                    return invalidCommand("command has trailing content");
                }
                return fields;
            }
        };

        // Converts a millisecond count into the monotonic Duration the host times
        // with, refusing one the clock's tick representation cannot hold.
        [[nodiscard]]
        auto millisDuration(
            uint64 millis,
            std::string_view field
        ) -> Result<MonotonicInstant::Duration>
        {
            using Duration     = MonotonicInstant::Duration;
            using Milliseconds = std::chrono::milliseconds;

            auto const maximum      = std::chrono::duration_cast<Milliseconds>(Duration::max());
            auto const maximumCount = checkedCast<uint64>(maximum.count());
            auto const count        = checkedCast<Milliseconds::rep>(millis);
            if (!maximumCount || millis > *maximumCount || !count)
            {
                return invalidCommand(
                    std::format("command field {} is too large", field)
                );
            }
            return std::chrono::duration_cast<Duration>(Milliseconds{*count});
        }

        // Reads one required millisecond field of a LAYER-ONE command, where it is
        // simply that primitive's argument.
        [[nodiscard]]
        auto requireMillis(
            std::optional<uint64> const& value,
            std::string_view field,
            std::string_view operation
        ) -> Result<MonotonicInstant::Duration>
        {
            if (!value)
            {
                return invalidCommand(
                    std::format(
                        "the {} command requires field {}",
                        operation,
                        field
                    )
                );
            }
            return millisDuration(*value, field);
        }

        [[nodiscard]]
        auto requireName(
            std::optional<std::string> const& value,
            std::string_view field,
            std::string_view operation
        ) -> Result<std::string>
        {
            if (!value || value->empty())
            {
                return invalidCommand(
                    std::format(
                        "the {} command requires a non-empty {} field",
                        operation,
                        field
                    )
                );
            }
            return *value;
        }

        [[nodiscard]]
        auto requireId(
            std::optional<uint64> const& value,
            std::string_view field,
            std::string_view operation
        ) -> Result<uint64>
        {
            if (!value)
            {
                return invalidCommand(
                    std::format(
                        "the {} command requires field {}",
                        operation,
                        field
                    )
                );
            }
            return *value;
        }

        // Which fields each op may carry. A field that belongs to another command is
        // refused rather than ignored: silently dropping "poll_ms" from a `key`
        // command would let an operator believe a pause it asked for happened.
        struct FieldUse final
        {
            bool key{false};
            bool cycle{false};
            bool deadline{false};
            bool millis{false};
            bool pollMillis{false};
        };

        [[nodiscard]]
        auto rejectUnusedFields(
            ParsedFields const& fields,
            FieldUse const& allowed,
            std::string_view operation
        ) -> Status
        {
            auto const refuse = [operation](std::string_view field) -> Status
            {
                return invalidCommand(
                    std::format(
                        "the {} command does not take field {}",
                        operation,
                        field
                    )
                );
            };

            if (fields.key && !allowed.key) { return refuse("key"); }
            if (fields.cycle && !allowed.cycle) { return refuse("cycle"); }
            if (fields.deadline && !allowed.deadline) { return refuse("deadline"); }
            if (fields.millis && !allowed.millis) { return refuse("ms"); }
            if (fields.pollMillis && !allowed.pollMillis) { return refuse("poll_ms"); }
            return ok();
        }

        [[nodiscard]]
        auto commandFromFields(ParsedFields fields) -> Result<DriveCommand>
        {
            if (!fields.operation)
            {
                return invalidCommand("command is missing field op");
            }
            auto const operation = *fields.operation;

            if (operation == "cycle_open")
            {
                UF_TRY(rejectUnusedFields(fields, FieldUse{}, operation));
                return DriveCycleOpenCommand{};
            }
            if (operation == "quit")
            {
                UF_TRY(rejectUnusedFields(fields, FieldUse{}, operation));
                return DriveQuitCommand{};
            }
            if (operation == "cycle_close")
            {
                UF_TRY(
                    rejectUnusedFields(fields, FieldUse{.cycle = true}, operation)
                );
                UF_TRY_VALUE(cycle, requireId(fields.cycle, "cycle", operation));
                return DriveCycleCloseCommand{.cycle = cycle};
            }
            if (operation == "key")
            {
                UF_TRY(
                    rejectUnusedFields(
                        fields,
                        FieldUse{.key = true, .cycle = true},
                        operation
                    )
                );
                UF_TRY_VALUE(cycle, requireId(fields.cycle, "cycle", operation));
                UF_TRY_VALUE(name, requireName(fields.key, "key", operation));
                UF_TRY_VALUE(keyName, KeyName::create(name));
                return DriveKeyCommand{.cycle = cycle, .key = keyName};
            }
            if (operation == "settle")
            {
                UF_TRY(
                    rejectUnusedFields(fields, FieldUse{.millis = true}, operation)
                );
                UF_TRY_VALUE(
                    duration,
                    requireMillis(fields.millis, "ms", operation)
                );
                return DriveSettleCommand{.duration = duration};
            }
            if (operation == "deadline")
            {
                UF_TRY(
                    rejectUnusedFields(fields, FieldUse{.millis = true}, operation)
                );
                UF_TRY_VALUE(
                    duration,
                    requireMillis(fields.millis, "ms", operation)
                );
                return DriveDeadlineCommand{.duration = duration};
            }
            if (operation == "wait")
            {
                UF_TRY(
                    rejectUnusedFields(
                        fields,
                        FieldUse{.deadline = true, .pollMillis = true},
                        operation
                    )
                );
                UF_TRY_VALUE(
                    deadline,
                    requireId(fields.deadline, "deadline", operation)
                );
                UF_TRY_VALUE(
                    pollInterval,
                    requireMillis(fields.pollMillis, "poll_ms", operation)
                );
                return DriveWaitCommand{
                    .deadline     = deadline,
                    .pollInterval = pollInterval,
                };
            }
            return invalidCommand(
                std::format("command has unrecognized op \"{}\"", operation)
            );
        }
    }

    auto parseDriveCommand(std::string_view line) -> Result<DriveCommand>
    {
        if (line.size() > k_maxDriveCommandBytes)
        {
            return invalidCommand(
                std::format(
                    "command exceeds the {}-byte limit",
                    k_maxDriveCommandBytes
                )
            );
        }
        if (!isValidUtf8(line))
        {
            return invalidCommand("command must contain valid UTF-8");
        }

        auto reader = CommandReader{line};
        UF_TRY_VALUE(fields, reader.parseFields());
        return commandFromFields(std::move(fields));
    }

    auto driveCommandOperation(DriveCommand const& command) -> std::string_view
    {
        return std::visit(
            [](auto const& specific) -> std::string_view
            {
                using Command = std::decay_t<decltype(specific)>;
                if constexpr (std::same_as<Command, DriveCycleOpenCommand>)
                {
                    return "cycle_open";
                }
                else if constexpr (std::same_as<Command, DriveCycleCloseCommand>)
                {
                    return "cycle_close";
                }
                else if constexpr (std::same_as<Command, DriveKeyCommand>)
                {
                    return "key";
                }
                else if constexpr (std::same_as<Command, DriveSettleCommand>)
                {
                    return "settle";
                }
                else if constexpr (std::same_as<Command, DriveDeadlineCommand>)
                {
                    return "deadline";
                }
                else if constexpr (std::same_as<Command, DriveWaitCommand>)
                {
                    return "wait";
                }
                else
                {
                    static_assert(std::same_as<Command, DriveQuitCommand>);
                    return "quit";
                }
            },
            command
        );
    }

    auto serializeDriveResult(
        std::string_view operation,
        DriveResult const& result
    ) -> std::string
    {
        auto line = std::string{"{\"op\":"};
        line += trace::escapeJsonString(operation);
        line += result.ok ? ",\"ok\":true" : ",\"ok\":false";

        if (result.cycle.has_value())
        {
            line += std::format(",\"cycle\":{}", *result.cycle);
        }
        if (result.deadline.has_value())
        {
            line += std::format(",\"deadline\":{}", *result.deadline);
        }
        if (result.released.has_value())
        {
            line += *result.released ? ",\"released\":true" : ",\"released\":false";
        }
        if (result.budget.has_value())
        {
            line += *result.budget ? ",\"budget\":true" : ",\"budget\":false";
        }
        if (result.errorKind.has_value())
        {
            line += ",\"error\":";
            line += trace::escapeJsonString(*result.errorKind);
        }
        if (result.message.has_value())
        {
            line += ",\"message\":";
            line += trace::escapeJsonString(*result.message);
        }

        line += '}';
        return line;
    }

    auto serializeDriveParseFailure(Error const& error) -> std::string
    {
        // The op is empty rather than guessed: the line did not successfully name a
        // command, and inventing one would attribute the refusal to a command the
        // operator may not have written.
        return serializeDriveResult(
            "",
            DriveResult{
                .errorKind = std::string{
                    automationErrorWireName(
                        automationErrorKind(error)
                            .value_or(AutomationErrorKind::InvalidResource)
                    )
                },
                .message = std::string{error.message()},
            }
        );
    }

    auto driveFailure(
        std::string_view operation,
        Error const& error
    ) -> std::string
    {
        return serializeDriveResult(
            operation,
            DriveResult{
                .errorKind = std::string{
                    automationErrorWireName(
                        automationErrorKind(error)
                            .value_or(AutomationErrorKind::InternalInvariant)
                    )
                },
                .message = std::string{error.message()},
            }
        );
    }
}
