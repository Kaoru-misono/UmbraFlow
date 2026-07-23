#include "test-helpers.hpp"

#include <annotation/content-hash.hpp>

#include <doctest/doctest.h>

#include <array>
#include <span>
#include <string>
#include <string_view>

namespace uf::annotation
{
    namespace
    {
        [[nodiscard]]
        auto hashText(std::string_view text) -> std::string
        {
            auto const hashed = sha256(std::as_bytes(std::span{text}));
            REQUIRE(hashed.has_value());
            return hashed->toString();
        }
    }

    TEST_CASE("annotation SHA-256 matches standard single and multi-block vectors")
    {
        CHECK(
            hashText("")
            == "sha256:e3b0c44298fc1c149afbf4c8996fb924"
               "27ae41e4649b934ca495991b7852b855"
        );
        CHECK(
            hashText("abc")
            == "sha256:ba7816bf8f01cfea414140de5dae2223"
               "b00361a396177a9cb410ff61f20015ad"
        );
        CHECK(
            hashText(
                "abcdbcdecdefdefgefghfghighijhijk"
                "ijkljklmklmnlmnomnopnopq"
            )
            == "sha256:248d6a61d20638b8e5c026930c3e6039"
               "a33ce45964ff2167f6ecedd419db06c1"
        );
    }

    TEST_CASE("annotation content hashes parse only canonical lowercase SHA-256")
    {
        auto const canonical = hashText("canonical");
        auto const parsed = ContentHash::parse(canonical);
        REQUIRE(parsed.has_value());
        CHECK(parsed->toString() == canonical);
        CHECK(parsed->hex() == canonical.substr(std::string_view{"sha256:"}.size()));

        for (auto const invalid : std::array{
            std::string{"sha256:abcd"},
            std::string{
                "sha256:E3B0C44298FC1C149AFBF4C8996FB924"
                "27AE41E4649B934CA495991B7852B855"
            },
            std::string{
                "sha512:e3b0c44298fc1c149afbf4c8996fb924"
                "27ae41e4649b934ca495991b7852b855"
            },
            std::string{
                "sha256:g3b0c44298fc1c149afbf4c8996fb924"
                "27ae41e4649b934ca495991b7852b855"
            },
        })
        {
            auto const rejected = ContentHash::parse(invalid);
            REQUIRE_FALSE(rejected.has_value());
            test::requireErrorKind(
                rejected.error(),
                AutomationErrorKind::InvalidResource
            );
        }
    }
}
