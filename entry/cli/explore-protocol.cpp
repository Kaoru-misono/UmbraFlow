#include "explore-protocol.hpp"

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/text/utf8.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <script/engine.hpp>

#include <trace/json-text.hpp>

#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace uf::cli
{
    namespace
    {
        [[nodiscard]]
        auto invalidLine(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        // A strict reader for the one JSON shape this protocol uses: a flat object
        // of exactly two string members. Deliberately not a reuse of the operator's
        // reader, which is strict about different things -- six optional members of
        // mixed type and a builder that decides which combination names a command
        // -- and sharing would mean a reader parameterised by a field table. What
        // IS shared is the half where a difference would silently corrupt a file
        // rather than fail: escapeJsonString, in trace/json-text.hpp.
        class LineReader final
        {
            std::string_view m_source;
            std::size_t      m_position{};

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

            auto skipWhitespace() noexcept -> void
            {
                while (auto const value = peek())
                {
                    if (*value != ' ' && *value != '\t')
                    {
                        return;
                    }
                    ++m_position;
                }
            }

            [[nodiscard]] auto expect(char wanted, std::string_view what) -> Status
            {
                skipWhitespace();
                if (peek() != wanted)
                {
                    return invalidLine(
                        std::format(
                            "expected '{}' while reading {}",
                            wanted,
                            what
                        )
                    );
                }
                ++m_position;
                return ok();
            }

            // Reads one JSON string. Only the six named escapes and \uXXXX for the
            // ASCII control range are accepted: a chunk arrives as text, and an
            // escape this reader guessed at would change the program that runs.
            [[nodiscard]] auto readString(std::string_view what) -> Result<std::string>
            {
                UF_TRY(expect('"', what));

                auto value = std::string{};
                while (true)
                {
                    if (atEnd())
                    {
                        return invalidLine(
                            std::format("unterminated string while reading {}", what)
                        );
                    }
                    auto const character = m_source[m_position];
                    ++m_position;

                    if (character == '"')
                    {
                        return value;
                    }
                    if (static_cast<unsigned char>(character) < 0x20U)
                    {
                        return invalidLine(
                            std::format(
                                "a raw control byte appears in {}; escape it",
                                what
                            )
                        );
                    }
                    if (character != '\\')
                    {
                        value += character;
                        continue;
                    }

                    if (atEnd())
                    {
                        return invalidLine(
                            std::format("unterminated escape while reading {}", what)
                        );
                    }
                    auto const escape = m_source[m_position];
                    ++m_position;
                    switch (escape)
                    {
                    case '"': value += '"'; break;
                    case '\\': value += '\\'; break;
                    case '/': value += '/'; break;
                    case 'b': value += '\b'; break;
                    case 'f': value += '\f'; break;
                    case 'n': value += '\n'; break;
                    case 'r': value += '\r'; break;
                    case 't': value += '\t'; break;
                    case 'u':
                    {
                        UF_TRY_VALUE(code, readFourHexDigits(what));
                        if (code > 0x7FU)
                        {
                            // Above ASCII a \u escape may name half of a surrogate
                            // pair, and decoding those is a second parser. A chunk
                            // needing a non-ASCII character carries it as UTF-8,
                            // which this reader passes through untouched.
                            return invalidLine(
                                std::format(
                                    "a \\u escape above U+007F appears in {}; "
                                    "write the character as UTF-8 instead",
                                    what
                                )
                            );
                        }
                        value += static_cast<char>(code);
                        break;
                    }
                    default:
                        return invalidLine(
                            std::format(
                                "unknown escape \\{} while reading {}",
                                escape,
                                what
                            )
                        );
                    }
                }
            }

            [[nodiscard]]
            auto readFourHexDigits(std::string_view what) -> Result<uint32>
            {
                auto value = uint32{0};
                for (auto digit = 0; digit < 4; ++digit)
                {
                    if (atEnd())
                    {
                        return invalidLine(
                            std::format("truncated \\u escape while reading {}", what)
                        );
                    }
                    auto const character = m_source[m_position];
                    ++m_position;

                    auto nibble = uint32{0};
                    if (character >= '0' && character <= '9')
                    {
                        nibble = static_cast<uint32>(character - '0');
                    }
                    else if (character >= 'a' && character <= 'f')
                    {
                        nibble = static_cast<uint32>(character - 'a') + 10U;
                    }
                    else if (character >= 'A' && character <= 'F')
                    {
                        nibble = static_cast<uint32>(character - 'A') + 10U;
                    }
                    else
                    {
                        return invalidLine(
                            std::format(
                                "a \\u escape has a non-hex digit while reading {}",
                                what
                            )
                        );
                    }
                    value = (value * 16U) + nibble;
                }
                return value;
            }

        public:
            explicit LineReader(std::string_view source) noexcept
                : m_source{source}
            {
            }

            [[nodiscard]] auto read() -> Result<ExploreChunk>
            {
                UF_TRY(expect('{', "the queue line"));

                auto id    = std::optional<std::string>{};
                auto chunk = std::optional<std::string>{};

                skipWhitespace();
                if (peek() != '}')
                {
                    while (true)
                    {
                        UF_TRY_VALUE(name, readString("a member name"));
                        UF_TRY(expect(':', "a member"));
                        UF_TRY_VALUE(
                            value,
                            readString(std::format("the '{}' member", name))
                        );

                        if (name == "id")
                        {
                            if (id.has_value())
                            {
                                return invalidLine(
                                    "the queue line repeats the 'id' member"
                                );
                            }
                            id = std::move(value);
                        }
                        else if (name == "chunk")
                        {
                            if (chunk.has_value())
                            {
                                return invalidLine(
                                    "the queue line repeats the 'chunk' member"
                                );
                            }
                            chunk = std::move(value);
                        }
                        else
                        {
                            return invalidLine(
                                std::format(
                                    "the queue line has an unrecognized member "
                                    "\"{}\"; it takes 'id' and 'chunk'",
                                    name
                                )
                            );
                        }

                        skipWhitespace();
                        if (peek() != ',')
                        {
                            break;
                        }
                        ++m_position;
                    }
                }

                UF_TRY(expect('}', "the queue line"));
                skipWhitespace();
                if (!atEnd())
                {
                    return invalidLine(
                        "the queue line has content after its closing brace"
                    );
                }

                if (!id.has_value())
                {
                    return invalidLine(
                        "the queue line has no 'id'; a session answers every "
                        "line and the id is what an answer is about"
                    );
                }
                if (!chunk.has_value())
                {
                    return invalidLine(
                        "the queue line has no 'chunk'; there is nothing to run"
                    );
                }
                if (id->empty())
                {
                    return invalidLine("the queue line's 'id' is empty");
                }
                if (id->size() > k_maxExploreIdBytes)
                {
                    return invalidLine(
                        std::format(
                            "the queue line's 'id' exceeds {} bytes",
                            k_maxExploreIdBytes
                        )
                    );
                }

                return ExploreChunk{
                    .id    = *std::move(id),
                    .chunk = *std::move(chunk),
                };
            }
        };
    }

    auto parseExploreChunk(std::string_view line) -> Result<ExploreChunk>
    {
        if (line.size() > k_maxExploreLineBytes)
        {
            return invalidLine(
                std::format(
                    "the queue line exceeds the {}-byte limit",
                    k_maxExploreLineBytes
                )
            );
        }
        if (!isValidUtf8(line))
        {
            return invalidLine("the queue line must contain valid UTF-8");
        }

        auto reader = LineReader{line};
        return reader.read();
    }

    auto serializeExploreResult(ExploreResult const& result) -> std::string
    {
        auto line = std::string{"{\"id\":"};
        line += trace::escapeJsonString(result.id);
        line += result.ok ? ",\"ok\":true" : ",\"ok\":false";

        if (result.boolean.has_value())
        {
            line += *result.boolean ? ",\"value\":true" : ",\"value\":false";
        }
        if (result.number.has_value())
        {
            // Seventeen significant digits round-trips every double exactly, which
            // matters because a chunk's answer may be a pixel count an agent then
            // feeds back as an argument.
            line += std::format(",\"value\":{:.17g}", *result.number);
        }
        if (result.text.has_value())
        {
            line += ",\"value\":";
            line += trace::escapeJsonString(*result.text);
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
        if (result.heap.has_value())
        {
            // A nested object rather than two flat members, because the figures are
            // only meaningful against each other: a line carrying `used` alone
            // would invite an agent to compare it against a ceiling it assumed.
            line += std::format(
                ",\"heap\":{{\"used\":{},\"ceiling\":{}}}",
                result.heap->usedBytes,
                result.heap->ceilingBytes
            );
        }

        line += '}';
        return line;
    }

    auto exploreSuccess(
        std::string_view id,
        script::ScriptValue const& value,
        script::HeapUsage heap
    ) -> std::string
    {
        auto result = ExploreResult{
            .id   = std::string{id},
            .ok   = true,
            .heap = heap,
        };

        // A chunk that returned nothing sets none of the three, so its line carries
        // no `value` member at all. That absence is the answer: `"value":null` is
        // not distinguishable in JSON from a member the writer forgot.
        if (auto const boolean = value.boolean(); boolean.has_value())
        {
            result.boolean = *boolean;
        }
        else if (auto const number = value.number(); number.has_value())
        {
            // `{:.17g}` renders a non-finite double as the bare token inf, -inf or
            // nan, none of which JSON can carry, so the line would not parse at
            // all -- and an agent may feed a numeric answer back as an argument.
            if (!std::isfinite(*number))
            {
                return exploreFailure(
                    id,
                    fail(
                        AutomationErrorKind::InvalidResource,
                        "a chunk answered with a value JSON cannot carry"
                    ).error(),
                    heap
                );
            }
            result.number = *number;
        }
        else if (auto const* p_text = value.text(); p_text != nullptr)
        {
            result.text = *p_text;
        }

        return serializeExploreResult(result);
    }

    auto exploreFailure(
        std::string_view id,
        Error const& error,
        script::HeapUsage heap
    ) -> std::string
    {
        return serializeExploreResult(
            ExploreResult{
                .id = std::string{id},
                .ok = false,
                .errorKind = std::string{
                    automationErrorWireName(
                        automationErrorKind(error)
                            .value_or(AutomationErrorKind::InternalInvariant)
                    )
                },
                .message = std::string{error.message()},
                .heap    = heap,
            }
        );
    }

    auto serializeExploreParseFailure(Error const& error) -> std::string
    {
        return serializeExploreResult(
            ExploreResult{
                .id = std::string{},
                .ok = false,
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
}
