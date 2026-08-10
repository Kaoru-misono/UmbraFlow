#include <core/text/utf8.hpp>

#include <doctest/doctest.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

TEST_CASE("UTF-8 validation accepts scalar encodings")
{
    auto constexpr cases = std::array{
        std::string_view{},
        std::string_view{"UmbraFlow"},
        std::string_view{"\xC2\xA2", 2},
        std::string_view{"\xE2\x82\xAC", 3},
        std::string_view{"\xF0\x9F\x98\x80", 4},
    };

    for (auto const value : cases)
    {
        CAPTURE(value.size());
        CHECK(uf::isValidUtf8(value));
    }
}

TEST_CASE("UTF-8 validation rejects malformed encodings")
{
    auto constexpr cases = std::array{
        std::string_view{"\x80", 1},
        std::string_view{"\xC0\x80", 2},
        std::string_view{"\xE2\x82", 2},
        std::string_view{"\xED\xA0\x80", 3},
        std::string_view{"\xF4\x90\x80\x80", 4},
    };

    for (auto const value : cases)
    {
        CAPTURE(value.size());
        CHECK_FALSE(uf::isValidUtf8(value));
    }
}

TEST_CASE("UTF-8 scalar encoding covers every sequence width")
{
    auto output = std::string{};
    uf::appendUtf8Scalar(output, 0x24U);
    uf::appendUtf8Scalar(output, 0xA2U);
    uf::appendUtf8Scalar(output, 0x20ACU);
    uf::appendUtf8Scalar(output, 0x1F600U);

    CHECK(
        output
        == std::string{"\x24\xC2\xA2\xE2\x82\xAC\xF0\x9F\x98\x80", 10}
    );
}

TEST_CASE("UTF-8 scalar decoding covers every sequence width")
{
    auto const decoded = uf::decodeUtf8Scalars(
        std::string_view{
            "\x24\xC2\xA2\xE2\x82\xAC\xF0\x9F\x98\x80",
            10,
        }
    );
    REQUIRE(decoded.has_value());
    auto const expected =
        std::vector<uf::uint32>{0x24U, 0xA2U, 0x20ACU, 0x1F600U};
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): REQUIRE above proved engagement.
    CHECK(*decoded == expected);
    CHECK_FALSE(
        uf::decodeUtf8Scalars(std::string_view{"\xED\xA0\x80", 3}).has_value()
    );
}
