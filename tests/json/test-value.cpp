#include "repository-path.hpp"

#include <json/error.hpp>
#include <json/value.hpp>

#include <core/types/integer.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::json
{
    namespace
    {
        [[nodiscard]] auto parsed(std::string_view text) -> Value
        {
            auto value = parse(text);
            REQUIRE(value.has_value());
            return *std::move(value);
        }

        [[nodiscard]] auto refuses(std::string_view text) -> bool
        {
            auto const value = parse(text);
            return !value.has_value()
                && errorKind(value.error()) == ErrorKind::Syntax;
        }

        [[nodiscard]] auto canonical(std::string_view text) -> std::string
        {
            return canonicalBytes(parsed(text));
        }
    }

    TEST_CASE("json::parse reads every JSON kind")
    {
        CHECK(parsed("null").kind() == ValueKind::Null);
        CHECK(parsed("true").boolean());
        CHECK_FALSE(parsed("false").boolean());
        CHECK(parsed("-12.5e2").number() == -1250.0);
        CHECK(parsed(R"("text")").string() == "text");
        CHECK(parsed("[1,2,3]").items().size() == 3U);
        CHECK(parsed(R"({"b":1,"a":2})").members().size() == 2U);
    }

    TEST_CASE("json::parse keeps members in the order the document spelled them")
    {
        auto const value = parsed(R"({"b":1,"a":2})");
        REQUIRE(value.members().size() == 2U);
        CHECK(value.members()[0].first == "b");
        CHECK(value.members()[1].first == "a");
        CHECK(value.find("a")->number() == 2.0);
        CHECK(value.find("missing") == nullptr);
    }

    TEST_CASE("json::parse resolves string escapes into UTF-8")
    {
        CHECK(parsed(R"("A")").string() == "A");
        CHECK(parsed(R"("😀")").string() == "\xf0\x9f\x98\x80");
        CHECK(parsed(R"("\"\\\/\b\f\n\r\t")").string() == "\"\\/\b\f\n\r\t");
    }

    TEST_CASE("json::parse refuses what a canonical document can never carry")
    {
        CHECK(refuses(""));
        CHECK(refuses("{} trailing"));
        CHECK(refuses(R"({"a":1,"a":2})"));
        CHECK(refuses("[1,]"));
        CHECK(refuses("{'a':1}"));
        CHECK(refuses("01"));
        CHECK(refuses("+1"));
        CHECK(refuses(".5"));
        CHECK(refuses("1."));
        CHECK(refuses("1e"));
        CHECK(refuses("NaN"));
        CHECK(refuses("1e400"));
        CHECK(refuses("1e-400"));
        CHECK(refuses("00"));
        CHECK(refuses("-01"));
        CHECK(refuses("012"));
        CHECK(refuses("\"unterminated"));
        CHECK(refuses("\"a\tb\""));
        CHECK(refuses(R"("\q")"));
        CHECK(refuses(R"("\ud800")"));
        CHECK(refuses(R"("\udc00")"));
        CHECK(refuses(R"("\ud800A")"));
        CHECK(refuses("\"\xff\""));
    }

    TEST_CASE("json::parse refuses a document deeper than its limit")
    {
        auto shallow = std::string(60U, '[') + std::string(60U, ']');
        auto deep    = std::string(200U, '[') + std::string(200U, ']');
        CHECK(parse(shallow).has_value());
        CHECK(refuses(deep));
    }

    TEST_CASE("json::Value equality compares values, not spellings")
    {
        CHECK(parsed(R"({"a":1,"b":2})") == parsed(R"({"b":2,"a":1})"));
        CHECK_FALSE(parsed(R"({"a":1})") == parsed(R"({"a":2})"));
        CHECK_FALSE(parsed("[1,2]") == parsed("[2,1]"));
        CHECK_FALSE(parsed("1") == parsed("true"));
        CHECK(parsed("1") == parsed("1.0"));
    }

    TEST_CASE("json::Value::isInteger answers for exact integers only")
    {
        CHECK(parsed("1").isInteger());
        CHECK(parsed("1.0").isInteger());
        CHECK(parsed("1e2").isInteger());
        CHECK(parsed("-0").isInteger());
        CHECK_FALSE(parsed("1.5").isInteger());
        CHECK_FALSE(parsed(R"("1")").isInteger());
    }

    TEST_CASE("json::requireExactCanonical refuses each way a document is not canonical")
    {
        CHECK(requireExactCanonical(R"({"a":1,"b":2})").has_value());
        CHECK(requireExactCanonical("[]").has_value());
        CHECK(requireExactCanonical("{}").has_value());

        auto const notCanonical = [](std::string_view text)
        {
            auto const outcome = requireExactCanonical(text);
            return !outcome.has_value()
                && errorKind(outcome.error()) == ErrorKind::NotCanonical;
        };

        // Member order.
        CHECK(notCanonical(R"({"b":1,"a":2})"));
        // Insignificant whitespace.
        CHECK(notCanonical(R"({"a": 1})"));
        CHECK(notCanonical("[1, 2]"));
        CHECK(notCanonical("{}\n"));
        // Number form.
        CHECK(notCanonical("1.0"));
        CHECK(notCanonical("1e2"));
        CHECK(notCanonical("-0"));
        CHECK(notCanonical("1E+2"));
        // String escaping: an escape RFC 8785 resolves away, the one it
        // spells differently, uppercase hex, and a surrogate pair a
        // canonical document writes out as UTF-8.
        CHECK(notCanonical(R"("\u0041")"));
        CHECK(notCanonical(R"("\/")"));
        CHECK(notCanonical(R"("\u000a")"));
        CHECK(notCanonical(R"("\u001F")"));
        CHECK(notCanonical(R"("\ud83d\ude00")"));

        // Duplicate members and invalid UTF-8 are refused one step earlier, by
        // the parser, so they never reach the byte comparison.
        auto const duplicated = requireExactCanonical(R"({"a":1,"a":1})");
        REQUIRE_FALSE(duplicated.has_value());
        CHECK(errorKind(duplicated.error()) == ErrorKind::Syntax);

        auto const notUtf8 = requireExactCanonical("\"\xff\"");
        REQUIRE_FALSE(notUtf8.has_value());
        CHECK(errorKind(notUtf8.error()) == ErrorKind::Syntax);
    }

    TEST_CASE("json::canonicalBytes writes RFC 8785 numbers")
    {
        CHECK(canonical("0") == "0");
        CHECK(canonical("-0") == "0");
        CHECK(canonical("1.0") == "1");
        CHECK(canonical("1e2") == "100");
        CHECK(canonical("1e20") == "100000000000000000000");
        CHECK(canonical("1e21") == "1e+21");
        CHECK(canonical("1e-6") == "0.000001");
        CHECK(canonical("1e-7") == "1e-7");
        CHECK(canonical("0.1") == "0.1");
        CHECK(canonical("123.456") == "123.456");
        CHECK(canonical("5e-324") == "5e-324");
    }

    TEST_CASE("json::canonicalBytes orders members by UTF-16 code units")
    {
        // U+10000 becomes a surrogate pair starting D800, so it sorts before
        // U+E000 -- which neither UTF-8 byte order nor code-point order does.
        CHECK(
            canonical("{\"\xee\x80\x80\":1,\"\xf0\x90\x80\x80\":2}")
            == "{\"\xf0\x90\x80\x80\":2,\"\xee\x80\x80\":1}"
        );
        CHECK(canonical(R"({"ab":1,"a":2,"b":3})") == R"({"a":2,"ab":1,"b":3})");
    }

    // ------------------------------------------------------------------
    // tests/vectors/jcs-vectors.txt.
    //
    // The file's cpp= column describes core/text/json-text.cpp, whose two
    // functions cover the string escape and member ordering only; every number
    // and container row therefore carries cpp=absent. This module is the other
    // half of RFC 8785 in C++, so it answers those rows too, and the assertions
    // below deliberately ignore an `absent` override. Any OTHER cpp override
    // would be a claim about C++ behaviour that this file must not silently
    // skip, so one is a failure rather than a skip.
    // ------------------------------------------------------------------

    namespace
    {
        constexpr auto k_vectorPath = std::string_view{"tests/vectors/jcs-vectors.txt"};

        struct VectorRow final
        {
            std::string                             kind{};
            std::vector<std::optional<std::string>> fields{};
            std::optional<std::string>              cppOverride{};
        };

        [[nodiscard]] auto decodeHex(std::string_view token) -> std::string
        {
            auto bytes = std::string{};
            for (auto at = std::size_t{1}; at + 1U < token.size() + 1U; at += 2U)
            {
                auto const digit = [](char character) -> uint32
                {
                    if (character >= '0' && character <= '9')
                    {
                        return static_cast<uint32>(character - '0');
                    }
                    return static_cast<uint32>(character - 'a') + 10U;
                };
                bytes.push_back(
                    static_cast<char>((digit(token[at]) << 4U) | digit(token[at + 1U]))
                );
            }
            return bytes;
        }

        struct VectorFile final
        {
            std::size_t            declaredCount{0};
            std::vector<VectorRow> rows{};
        };

        [[nodiscard]] auto readVectors() -> VectorFile
        {
            auto file = VectorFile{};
            auto stream =
                std::ifstream{repositoryRoot(k_vectorPath) / k_vectorPath, std::ios::binary};
            REQUIRE(stream.is_open());

            auto line = std::string{};
            while (std::getline(stream, line))
            {
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                {
                    line.pop_back();
                }
                if (line.empty() || line.front() == '#')
                {
                    continue;
                }

                auto tokens = std::vector<std::string>{};
                auto words  = std::istringstream{line};
                auto word   = std::string{};
                while (words >> word)
                {
                    if (word == "#")
                    {
                        break;
                    }
                    tokens.emplace_back(word);
                }
                if (tokens.empty())
                {
                    continue;
                }
                if (tokens.front() == "count")
                {
                    file.declaredCount = std::stoull(tokens[1]);
                    continue;
                }

                auto row = VectorRow{.kind = tokens.front()};
                for (auto index = std::size_t{1}; index < tokens.size(); ++index)
                {
                    auto const& token = tokens[index];
                    if (token.starts_with("cpp="))
                    {
                        row.cppOverride = token.substr(4);
                        continue;
                    }
                    if (token.contains('='))
                    {
                        continue;
                    }
                    row.fields.emplace_back(
                        token == "-" ? std::optional<std::string>{}
                                     : std::optional<std::string>{decodeHex(token)}
                    );
                }
                file.rows.emplace_back(std::move(row));
            }
            return file;
        }
    }

    TEST_CASE("json canonical bytes agree with the shared RFC 8785 vectors")
    {
        auto const file = readVectors();
        REQUIRE(file.declaredCount != 0U);
        REQUIRE(file.rows.size() == file.declaredCount);

        auto answered = std::size_t{0};
        for (auto const& row : file.rows)
        {
            CAPTURE(row.kind);
            // Only "absent" is a statement this module is entitled to override.
            CHECK((!row.cppOverride.has_value() || *row.cppOverride == "absent"));

            if (row.kind == "string")
            {
                REQUIRE(row.fields.size() == 2U);
                REQUIRE(row.fields[1].has_value());
                CHECK(
                    canonicalBytes(Value::ofString(row.fields[0].value_or(std::string{})))
                    == *row.fields[1]
                );
                ++answered;
                continue;
            }
            if (row.kind == "order")
            {
                REQUIRE(row.fields.size() >= 2U);
                // Members are supplied in reverse so the encoder has to sort
                // rather than preserve, and each value is the name's 1-based
                // canonical position, which is what the file's expected bytes
                // carry.
                auto members = std::vector<Member>{};
                for (auto index = row.fields.size(); index > 1U; --index)
                {
                    members.emplace_back(
                        row.fields[index - 1U].value_or(std::string{}),
                        Value::ofNumber(static_cast<double>(index - 1U))
                    );
                }
                // A row whose expected field is - asserts nothing through it.
                // What such a row still states is the ORDER of its name list,
                // so the expected bytes are assembled from that list rather
                // than read from the file.
                auto expected = row.fields[0];
                if (!expected.has_value())
                {
                    auto assembled = std::string{"{"};
                    for (auto index = std::size_t{1};
                         index < row.fields.size();
                         ++index)
                    {
                        if (index > 1U)
                        {
                            assembled += ',';
                        }
                        assembled += '"';
                        assembled += row.fields[index].value_or(std::string{});
                        assembled += "\":";
                        assembled += std::to_string(index);
                    }
                    assembled += '}';
                    expected = assembled;
                }
                CHECK(canonicalBytes(Value::ofObject(std::move(members)))
                      == *expected);
                ++answered;
                continue;
            }
            if (row.kind == "integer" || row.kind == "double")
            {
                REQUIRE(row.fields.size() == 2U);
                REQUIRE(row.fields[0].has_value());
                auto const& literal = *row.fields[0];
                if (literal == "nan" || literal == "inf" || literal == "-inf")
                {
                    CHECK_FALSE(parse(literal).has_value());
                    ++answered;
                    continue;
                }
                REQUIRE(row.fields[1].has_value());
                CHECK(canonicalBytes(parsed(literal)) == *row.fields[1]);
                ++answered;
                continue;
            }
            if (row.kind == "empty-object")
            {
                CHECK(canonicalBytes(Value::ofObject({})) == "{}");
                ++answered;
                continue;
            }
            if (row.kind == "empty-array")
            {
                CHECK(canonicalBytes(Value::ofArray({})) == "[]");
                ++answered;
                continue;
            }

            FAIL("unknown vector row kind: " << row.kind);
        }

        // Every row is answered here, including the 27 the file marks
        // cpp=absent because core/text/json-text.cpp has no value-tree encoder.
        CHECK(answered == file.declaredCount);
    }
}
