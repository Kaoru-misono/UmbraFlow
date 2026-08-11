#include "value.hpp"

#include "error.hpp"

#include <core/error/contracts.hpp>
#include <core/text/json-text.hpp>
#include <core/text/utf8.hpp>
#include <core/types/integer.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace uf::json
{
    namespace
    {
        // Deep enough for every document either tree's schemas describe -- the
        // deepest is 9 -- and shallow enough that a hostile one is refused
        // before it exhausts the stack. Parsing is recursive, and so are
        // canonicalBytes and ~Value, so this bound is what keeps all three off
        // an unbounded descent.
        constexpr auto k_maximumDepth = std::size_t{64};

        // Long enough for the widest shortest-round-trip double, which is
        // 24 characters (-1.7976931348623157e+308).
        constexpr auto k_numberBufferSize = std::size_t{64};

        [[nodiscard]]
        auto syntax(std::string message) -> std::unexpected<Error>
        {
            return fail(ErrorKind::Syntax, std::move(message));
        }

        class Reader final
        {
            std::string_view m_text;
            std::size_t      m_at{0};

        public:
            explicit Reader(std::string_view text) noexcept
                : m_text{text}
            {
            }

            [[nodiscard]] auto exhausted() const noexcept -> bool
            {
                return m_at >= m_text.size();
            }

            [[nodiscard]] auto peek() const noexcept -> char
            {
                return exhausted() ? '\0' : m_text[m_at];
            }

            auto advance() noexcept -> void { ++m_at; }

            // Insignificant whitespace is a parse concern rather than a
            // canonical one: requireExactCanonical refuses any document that
            // carried some, because the re-serialization it compares against
            // has none.
            auto skipWhitespace() noexcept -> void
            {
                while (!exhausted())
                {
                    auto const character = m_text[m_at];
                    if (
                        character != ' '
                        && character != '\t'
                        && character != '\n'
                        && character != '\r'
                    )
                    {
                        return;
                    }
                    ++m_at;
                }
            }

            [[nodiscard]] auto parseValue(std::size_t depth) -> Result<Value>;

        private:
            [[nodiscard]] auto parseString() -> Result<std::string>;
            [[nodiscard]] auto parseNumber() -> Result<Value>;
            [[nodiscard]] auto parseArray(std::size_t depth) -> Result<Value>;
            [[nodiscard]] auto parseObject(std::size_t depth) -> Result<Value>;
            [[nodiscard]] auto parseLiteral(std::string_view word) -> Status;
            [[nodiscard]] auto parseHexQuad() -> Result<uint32>;
        };

        auto Reader::parseHexQuad() -> Result<uint32>
        {
            auto value = uint32{0};
            for (auto index = std::size_t{0}; index < 4U; ++index)
            {
                if (exhausted())
                {
                    return syntax("a \\u escape ended before its four digits");
                }

                auto const character = m_text[m_at];
                auto       digit     = uint32{0};
                if (character >= '0' && character <= '9')
                {
                    digit = static_cast<uint32>(character - '0');
                }
                else if (character >= 'a' && character <= 'f')
                {
                    digit = static_cast<uint32>(character - 'a') + 10U;
                }
                else if (character >= 'A' && character <= 'F')
                {
                    digit = static_cast<uint32>(character - 'A') + 10U;
                }
                else
                {
                    return syntax("a \\u escape carries a non-hexadecimal digit");
                }

                value = (value * 16U) + digit;
                ++m_at;
            }

            return value;
        }

        auto Reader::parseString() -> Result<std::string>
        {
            if (peek() != '"')
            {
                return syntax("expected a JSON string");
            }
            advance();

            auto text = std::string{};
            while (true)
            {
                if (exhausted())
                {
                    return syntax("a JSON string is unterminated");
                }

                auto const character = m_text[m_at];
                if (character == '"')
                {
                    advance();
                    break;
                }
                if (static_cast<uint8>(character) < 0x20U)
                {
                    return syntax("a JSON string carries an unescaped control byte");
                }
                if (character != '\\')
                {
                    text.push_back(character);
                    advance();
                    continue;
                }

                advance();
                if (exhausted())
                {
                    return syntax("a JSON string ends inside an escape");
                }

                auto const escape = m_text[m_at];
                advance();
                switch (escape)
                {
                case '"': text.push_back('"'); break;
                case '\\': text.push_back('\\'); break;
                case '/': text.push_back('/'); break;
                case 'b': text.push_back('\b'); break;
                case 'f': text.push_back('\f'); break;
                case 'n': text.push_back('\n'); break;
                case 'r': text.push_back('\r'); break;
                case 't': text.push_back('\t'); break;
                case 'u':
                {
                    UF_TRY_VALUE(first, parseHexQuad());
                    if (first >= 0xDC00U && first <= 0xDFFFU)
                    {
                        return syntax("a JSON string carries a lone low surrogate");
                    }
                    if (first < 0xD800U || first > 0xDBFFU)
                    {
                        appendUtf8Scalar(text, first);
                        break;
                    }

                    if (
                        m_at + 1U >= m_text.size()
                        || m_text[m_at] != '\\'
                        || m_text[m_at + 1U] != 'u'
                    )
                    {
                        return syntax("a JSON string carries a lone high surrogate");
                    }
                    m_at += 2U;

                    UF_TRY_VALUE(second, parseHexQuad());
                    if (second < 0xDC00U || second > 0xDFFFU)
                    {
                        return syntax(
                            "a surrogate pair is not completed by a low surrogate"
                        );
                    }

                    appendUtf8Scalar(
                        text,
                        0x10000U + ((first - 0xD800U) << 10U) + (second - 0xDC00U)
                    );
                    break;
                }
                default: return syntax("a JSON string carries an unknown escape");
                }
            }

            if (!isValidUtf8(text))
            {
                return syntax("a JSON string is not valid UTF-8");
            }

            return text;
        }

        auto Reader::parseNumber() -> Result<Value>
        {
            auto const start = m_at;
            if (peek() == '-')
            {
                advance();
            }
            if (exhausted())
            {
                return syntax("a JSON number ends after its sign");
            }

            // The integer part is read first and judged afterwards. Refusing a
            // leading zero inside the loop instead -- by admitting a lone '0'
            // in one branch and 1-9 in another -- reads like a check and is
            // not one: the lone-zero branch consumes the '0' before the other
            // branch can ever see it, so the leading zero is left to be caught
            // by whatever the '1' turns out to be next to.
            auto const integerStart = m_at;
            if (peek() < '0' || peek() > '9')
            {
                return syntax("a JSON number has no integer part");
            }
            auto const leadingDigit = peek();
            while (peek() >= '0' && peek() <= '9')
            {
                advance();
            }
            if (leadingDigit == '0' && m_at - integerStart > 1U)
            {
                return syntax("a JSON number has a leading zero");
            }

            if (peek() == '.')
            {
                advance();
                if (peek() < '0' || peek() > '9')
                {
                    return syntax("a JSON number has no digit after its point");
                }
                while (peek() >= '0' && peek() <= '9')
                {
                    advance();
                }
            }

            if (peek() == 'e' || peek() == 'E')
            {
                advance();
                if (peek() == '+' || peek() == '-')
                {
                    advance();
                }
                if (peek() < '0' || peek() > '9')
                {
                    return syntax("a JSON number has no exponent digits");
                }
                while (peek() >= '0' && peek() <= '9')
                {
                    advance();
                }
            }

            auto const token  = m_text.substr(start, m_at - start);
            auto       value  = double{0.0};
            auto const parsed = std::from_chars(
                token.data(),
                token.data() + token.size(),
                value
            );
            // The scan above already accepted the grammar, so the only failure
            // left is a magnitude with no double: too large to represent, or
            // too small to tell from zero. Both are refused rather than folded,
            // and for the same reason -- neither can occur in a document this
            // module judges. An overflowing literal has no JSON form at all,
            // and an underflowing one is not the canonical spelling of the zero
            // it would fold to, so requireExactCanonical would refuse it one
            // step later anyway. One rule covers both; a branch that folded the
            // second would be a second spelling of zero.
            if (
                parsed.ec != std::errc{}
                || parsed.ptr != token.data() + token.size()
            )
            {
                return syntax("a JSON number has no double this reader represents");
            }

            return Value::ofNumber(value);
        }

        auto Reader::parseLiteral(std::string_view word) -> Status
        {
            if (!m_text.substr(m_at).starts_with(word))
            {
                return fail(ErrorKind::Syntax, "expected a JSON literal");
            }

            m_at += word.size();
            return ok();
        }

        auto Reader::parseArray(std::size_t depth) -> Result<Value>
        {
            advance();
            auto items = std::vector<Value>{};
            skipWhitespace();
            if (peek() == ']')
            {
                advance();
                return Value::ofArray(std::move(items));
            }

            while (true)
            {
                skipWhitespace();
                UF_TRY_VALUE(item, parseValue(depth + 1U));
                items.emplace_back(std::move(item));
                skipWhitespace();
                if (peek() == ',')
                {
                    advance();
                    continue;
                }
                if (peek() == ']')
                {
                    advance();
                    return Value::ofArray(std::move(items));
                }

                return syntax("a JSON array is missing a comma or its close");
            }
        }

        auto Reader::parseObject(std::size_t depth) -> Result<Value>
        {
            advance();
            auto members = std::vector<Member>{};
            skipWhitespace();
            if (peek() == '}')
            {
                advance();
                return Value::ofObject(std::move(members));
            }

            while (true)
            {
                skipWhitespace();
                UF_TRY_VALUE(name, parseString());
                auto const repeated = std::ranges::any_of(
                    members,
                    [&name](Member const& member) { return member.first == name; }
                );
                if (repeated)
                {
                    return syntax("a JSON object repeats a member name");
                }

                skipWhitespace();
                if (peek() != ':')
                {
                    return syntax("a JSON member has no colon");
                }
                advance();
                skipWhitespace();

                UF_TRY_VALUE(value, parseValue(depth + 1U));
                members.emplace_back(std::move(name), std::move(value));
                skipWhitespace();
                if (peek() == ',')
                {
                    advance();
                    continue;
                }
                if (peek() == '}')
                {
                    advance();
                    return Value::ofObject(std::move(members));
                }

                return syntax("a JSON object is missing a comma or its close");
            }
        }

        auto Reader::parseValue(std::size_t depth) -> Result<Value>
        {
            if (depth > k_maximumDepth)
            {
                return syntax("a JSON document nests deeper than this reader allows");
            }
            if (exhausted())
            {
                return syntax("a JSON document ended where a value was expected");
            }

            switch (peek())
            {
            case '{': return parseObject(depth);
            case '[': return parseArray(depth);
            case '"':
            {
                UF_TRY_VALUE(text, parseString());
                return Value::ofString(std::move(text));
            }
            case 't':
                UF_TRY(parseLiteral("true"));
                return Value::ofBoolean(true);
            case 'f':
                UF_TRY(parseLiteral("false"));
                return Value::ofBoolean(false);
            case 'n':
                UF_TRY(parseLiteral("null"));
                return Value{};
            default: return parseNumber();
            }
        }

        // ES6 Number::toString, which RFC 8785 section 3.2.2.3 adopts whole.
        // Neither std::format nor std::to_chars produces it directly: both
        // choose an exponent threshold of their own, so the shortest digits and
        // the decimal exponent are taken from to_chars and then placed by the
        // ES6 rules. n below is ES6's n, the position of the decimal point
        // relative to the digit string, and k is its k, the digit count.
        auto appendNumber(std::string& output, double value) -> void
        {
            if (value == 0.0)
            {
                // Covers -0.0, which RFC 8785 serializes as 0.
                output += '0';
                return;
            }

            auto magnitudeOf = value;
            if (value < 0.0)
            {
                output += '-';
                magnitudeOf = -value;
            }

            auto       buffer  = std::array<char, k_numberBufferSize>{};
            auto const written = std::to_chars(
                buffer.data(),
                buffer.data() + buffer.size(),
                magnitudeOf,
                std::chars_format::scientific
            );
            UF_CHECK(written.ec == std::errc{});

            auto const text = std::string_view{
                buffer.data(),
                static_cast<std::size_t>(written.ptr - buffer.data()),
            };
            auto const exponentAt = text.find('e');
            UF_CHECK(exponentAt != std::string_view::npos);

            auto digits = std::string{};
            for (auto const character : text.substr(0U, exponentAt))
            {
                if (character != '.')
                {
                    digits.push_back(character);
                }
            }

            // to_chars writes the exponent sign, and from_chars refuses a
            // leading '+', so the sign is read here rather than handed over.
            auto exponentDigitsAt = exponentAt + 1U;
            auto exponentNegative = false;
            if (text[exponentDigitsAt] == '+' || text[exponentDigitsAt] == '-')
            {
                exponentNegative = text[exponentDigitsAt] == '-';
                ++exponentDigitsAt;
            }

            auto exponentMagnitude = 0;
            static_cast<void>(std::from_chars(
                text.data() + exponentDigitsAt,
                text.data() + text.size(),
                exponentMagnitude
            ));

            auto const k = static_cast<int>(digits.size());
            auto const n =
                (exponentNegative ? -exponentMagnitude : exponentMagnitude) + 1;

            if (k <= n && n <= 21)
            {
                output += digits;
                output.append(static_cast<std::size_t>(n - k), '0');
                return;
            }
            if (0 < n && n <= 21)
            {
                output += digits.substr(0U, static_cast<std::size_t>(n));
                output += '.';
                output += digits.substr(static_cast<std::size_t>(n));
                return;
            }
            if (-6 < n && n <= 0)
            {
                output += "0.";
                output.append(static_cast<std::size_t>(-n), '0');
                output += digits;
                return;
            }

            output += digits.substr(0U, 1U);
            if (k > 1)
            {
                output += '.';
                output += digits.substr(1U);
            }
            output += 'e';
            output += (n - 1 >= 0) ? '+' : '-';
            output += std::to_string(n - 1 >= 0 ? n - 1 : -(n - 1));
        }

        auto appendCanonical(std::string& output, Value const& value) -> void
        {
            switch (value.kind())
            {
            case ValueKind::Null: output += "null"; return;
            case ValueKind::Boolean:
                output += value.boolean() ? "true" : "false";
                return;
            case ValueKind::Number: appendNumber(output, value.number()); return;
            case ValueKind::String: appendJsonString(output, value.string()); return;
            case ValueKind::Array:
            {
                output += '[';
                auto separated = false;
                for (auto const& item : value.items())
                {
                    if (separated)
                    {
                        output += ',';
                    }
                    separated = true;
                    appendCanonical(output, item);
                }
                output += ']';
                return;
            }
            case ValueKind::Object:
            {
                auto ordered = std::vector<Member const*>{};
                ordered.reserve(value.members().size());
                for (auto const& member : value.members())
                {
                    ordered.emplace_back(&member);
                }
                std::ranges::sort(
                    ordered,
                    [](Member const* p_left, Member const* p_right)
                    {
                        return jsonMemberNameLess(p_left->first, p_right->first);
                    }
                );

                output += '{';
                auto separated = false;
                for (auto const* const p_member : ordered)
                {
                    if (separated)
                    {
                        output += ',';
                    }
                    separated = true;
                    appendJsonString(output, p_member->first);
                    output += ':';
                    appendCanonical(output, p_member->second);
                }
                output += '}';
                return;
            }
            }

            UF_UNREACHABLE_MSG("Unknown json::ValueKind value");
        }
    }

    auto Value::ofBoolean(bool value) -> Value
    {
        auto result      = Value{};
        result.m_storage = value;
        return result;
    }

    auto Value::ofNumber(double value) -> Value
    {
        auto result      = Value{};
        result.m_storage = value;
        return result;
    }

    auto Value::ofString(std::string value) -> Value
    {
        auto result      = Value{};
        result.m_storage = std::move(value);
        return result;
    }

    auto Value::ofArray(std::vector<Value> items) -> Value
    {
        auto result      = Value{};
        result.m_storage = std::move(items);
        return result;
    }

    auto Value::ofObject(std::vector<Member> members) -> Value
    {
        auto result      = Value{};
        result.m_storage = std::move(members);
        return result;
    }

    auto Value::kind() const noexcept -> ValueKind
    {
        return static_cast<ValueKind>(m_storage.index());
    }

    auto Value::boolean() const noexcept -> bool
    {
        auto const* const p_value = std::get_if<bool>(&m_storage);
        return p_value != nullptr && *p_value;
    }

    auto Value::number() const noexcept -> double
    {
        auto const* const p_value = std::get_if<double>(&m_storage);
        return p_value == nullptr ? 0.0 : *p_value;
    }

    auto Value::string() const noexcept -> std::string_view
    {
        auto const* const p_value = std::get_if<std::string>(&m_storage);
        return p_value == nullptr ? std::string_view{} : std::string_view{*p_value};
    }

    auto Value::items() const noexcept -> std::span<Value const>
    {
        auto const* const p_value = std::get_if<std::vector<Value>>(&m_storage);
        return p_value == nullptr ? std::span<Value const>{} : std::span{*p_value};
    }

    auto Value::members() const noexcept -> std::span<Member const>
    {
        auto const* const p_value = std::get_if<std::vector<Member>>(&m_storage);
        return p_value == nullptr ? std::span<Member const>{} : std::span{*p_value};
    }

    auto Value::isInteger() const noexcept -> bool
    {
        auto const* const p_value = std::get_if<double>(&m_storage);
        return p_value != nullptr && std::isfinite(*p_value)
            && *p_value == std::trunc(*p_value);
    }

    auto Value::find(std::string_view name) const -> Value const*
    {
        for (auto const& member : members())
        {
            if (member.first == name)
            {
                return &member.second;
            }
        }

        return nullptr;
    }

    auto Value::operator==(Value const& other) const -> bool
    {
        if (kind() != other.kind())
        {
            return false;
        }

        switch (kind())
        {
        case ValueKind::Null: return true;
        case ValueKind::Boolean: return boolean() == other.boolean();
        case ValueKind::Number: return number() == other.number();
        case ValueKind::String: return string() == other.string();
        case ValueKind::Array:
            return std::ranges::equal(items(), other.items());
        case ValueKind::Object:
        {
            if (members().size() != other.members().size())
            {
                return false;
            }
            for (auto const& member : members())
            {
                auto const* const p_other = other.find(member.first);
                if (p_other == nullptr || !(member.second == *p_other))
                {
                    return false;
                }
            }
            return true;
        }
        }

        UF_UNREACHABLE_MSG("Unknown json::ValueKind value");
    }

    auto parse(std::string_view text) -> Result<Value>
    {
        auto reader = Reader{text};
        reader.skipWhitespace();
        UF_TRY_VALUE(value, reader.parseValue(0U));
        reader.skipWhitespace();
        if (!reader.exhausted())
        {
            return syntax("a JSON document carries content after its root value");
        }

        return value;
    }

    auto canonicalBytes(Value const& value) -> std::string
    {
        auto output = std::string{};
        appendCanonical(output, value);
        return output;
    }

    auto requireExactCanonical(std::string_view text) -> Status
    {
        UF_TRY_VALUE(value, parse(text));
        if (canonicalBytes(value) != text)
        {
            return fail(
                ErrorKind::NotCanonical,
                "these bytes parse as JSON but are not their own RFC 8785 form"
            );
        }

        return ok();
    }
}
