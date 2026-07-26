#include "test-helpers.hpp"

#include <annotation/template-asset.hpp>

#include <core/types/integer.hpp>

#include <image/png.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <vector>

namespace uf::annotation
{
    namespace
    {
        [[nodiscard]]
        constexpr auto asByte(uint8 value) noexcept -> std::byte
        {
            return static_cast<std::byte>(value);
        }
    }

    TEST_CASE("annotation template generation crops, encodes, and content-addresses pixels")
    {
        auto const source = std::vector<std::byte>{
            asByte(1), asByte(2), asByte(3), asByte(255),
            asByte(4), asByte(5), asByte(6), asByte(255),
            asByte(7), asByte(8), asByte(9), asByte(255),
            asByte(0xEE), asByte(0xEE), asByte(0xEE), asByte(0xEE),
            asByte(10), asByte(11), asByte(12), asByte(255),
            asByte(13), asByte(14), asByte(15), asByte(255),
            asByte(16), asByte(17), asByte(18), asByte(255),
            asByte(0xEE), asByte(0xEE), asByte(0xEE), asByte(0xEE),
        };
        auto const rect = test::pixelRect(1, 0, 2, 2);

        auto const first = generateTemplateAsset(source, 3, 2, 16, rect);
        auto const second = generateTemplateAsset(source, 3, 2, 16, rect);
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        CHECK(first->pngBytes == second->pngBytes);
        CHECK(first->hash == second->hash);
        CHECK(first->relativePath == second->relativePath);
        CHECK(first->width == 2U);
        CHECK(first->height == 2U);
        CHECK(
            first->hash.toString()
            == "sha256:a95f5febce25a81632baccd921ab260e"
               "2d02127776c2649201ac6e5380cc3fec"
        );

        auto const recomputed = sha256(first->pngBytes);
        REQUIRE(recomputed.has_value());
        CHECK(*recomputed == first->hash);
        CHECK(
            first->relativePath
            == "assets/templates/" + first->hash.hex() + ".png"
        );

        auto const decoded = image::decodePng(
            first->pngBytes,
            first->relativePath
        );
        REQUIRE(decoded.has_value());
        CHECK(decoded->width == 2U);
        CHECK(decoded->height == 2U);
        auto const expected = std::vector<std::byte>{
            asByte(6), asByte(5), asByte(4), asByte(255),
            asByte(9), asByte(8), asByte(7), asByte(255),
            asByte(15), asByte(14), asByte(13), asByte(255),
            asByte(18), asByte(17), asByte(16), asByte(255),
        };
        CHECK(decoded->pixels == expected);
    }

    TEST_CASE("annotation template generation rejects crops outside source geometry")
    {
        auto const source = std::vector<std::byte>(
            std::size_t{3} * 2U * 4U
        );
        auto const rejected = generateTemplateAsset(
            source,
            3,
            2,
            12,
            test::pixelRect(2, 0, 2, 1)
        );
        REQUIRE_FALSE(rejected.has_value());
        test::requireErrorKind(
            rejected.error(),
            AutomationErrorKind::InvalidResource
        );
    }
}
