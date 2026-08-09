#include <core/text/json-text.hpp>

#include <doctest/doctest.h>

#include <array>
#include <string>
#include <string_view>

namespace
{
    struct EscapeCase final
    {
        std::string_view input;
        std::string_view expected;
    };

    [[nodiscard]]
    auto escaped(std::string_view value) -> std::string
    {
        auto output = std::string{};
        uf::appendJsonString(output, value);
        return output;
    }
}

TEST_CASE("JSON string escaping emits the exact RFC 8785 spellings")
{
    auto constexpr cases = std::array{
        EscapeCase{"", "\"\""},
        EscapeCase{"plain", "\"plain\""},
        EscapeCase{"\"", "\"\\\"\""},
        EscapeCase{"\\", "\"\\\\\""},
        EscapeCase{"\b\t\n\f\r", "\"\\b\\t\\n\\f\\r\""},

        // Every other C0 byte takes \u00xx in LOWERCASE hex. Uppercase would
        // parse identically and hash differently, which is the whole reason
        // this transform has one home.
        EscapeCase{std::string_view{"\x00", 1}, "\"\\u0000\""},
        EscapeCase{std::string_view{"\x1F", 1}, "\"\\u001f\""},
        EscapeCase{std::string_view{"\x0B", 1}, "\"\\u000b\""},

        // Bytes JCS forbids escaping: DEL, the solidus, and anything above
        // ASCII travel through as themselves.
        EscapeCase{std::string_view{"\x7F", 1}, std::string_view{"\"\x7F\"", 3}},
        EscapeCase{"a/b", "\"a/b\""},
        EscapeCase{"\xE2\x82\xAC", "\"\xE2\x82\xAC\""},
    };

    for (auto const& value : cases)
    {
        CAPTURE(value.expected);
        CHECK(escaped(value.input) == value.expected);
    }
}

TEST_CASE("JSON string escaping appends rather than replaces")
{
    auto output = std::string{"{\"k\":"};
    uf::appendJsonString(output, "v");
    output += '}';
    CHECK(output == "{\"k\":\"v\"}");
}
