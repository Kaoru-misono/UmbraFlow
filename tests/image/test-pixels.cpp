#include "test-helpers.hpp"

#include <image/pixels.hpp>

#include <core/types/integer.hpp>
#include <vision/sad.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <utility>
#include <vector>

namespace uf::image
{
    namespace
    {
        [[nodiscard]]
        constexpr auto asByte(uint8 value) noexcept -> std::byte
        {
            return static_cast<std::byte>(value);
        }
    }

    TEST_CASE("image RGBA8 to BGRA8 swaps red and blue")
    {
        auto rgba = std::vector<std::byte>{
            asByte(10),
            asByte(20),
            asByte(30),
            asByte(40),
            asByte(50),
            asByte(60),
            asByte(70),
            asByte(80),
        };
        auto const expected = std::vector<std::byte>{
            asByte(30),
            asByte(20),
            asByte(10),
            asByte(40),
            asByte(70),
            asByte(60),
            asByte(50),
            asByte(80),
        };
        auto const converted = rgba8ToBgra8(std::move(rgba));
        REQUIRE(converted.has_value());
        CHECK(*converted == expected);
    }

    TEST_CASE("image channel conversion rejects incomplete pixels")
    {
        auto const converted = rgba8ToBgra8(
            std::vector<std::byte>{asByte(1), asByte(2), asByte(3)}
        );
        REQUIRE_FALSE(converted.has_value());
        test_image::requireErrorKind(
            converted.error(),
            AutomationErrorKind::InvalidResource
        );
    }

    TEST_CASE("image template grayscale uses the frame grayscale kernel")
    {
        auto rgba = std::vector<std::byte>{
            asByte(200),
            asByte(89),
            asByte(17),
            asByte(255),
        };
        auto const nativeBgra = std::array{
            asByte(17),
            asByte(89),
            asByte(200),
            asByte(255),
        };
        auto const viaFrame = bgra8ToGray8(nativeBgra, 1, 1, 4);
        auto const bgra = rgba8ToBgra8(std::move(rgba));
        REQUIRE(bgra.has_value());
        auto const viaTemplate = bgra8ToGray8(*bgra, 1, 1, 4);
        REQUIRE(viaFrame.has_value());
        REQUIRE(viaTemplate.has_value());
        CHECK(*viaFrame == *viaTemplate);
    }

    TEST_CASE("image BGRA8 crop honors stride and packs tightly")
    {
        auto constexpr width = std::size_t{3};
        auto constexpr stride = width * 4U + 8U;
        auto source = std::vector<std::byte>(stride * 3U, asByte(0xEE));
        for (auto y = std::size_t{0}; y < 3U; ++y)
        {
            for (auto x = std::size_t{0}; x < 3U; ++x)
            {
                auto const offset = y * stride + x * 4U;
                source.at(offset) = asByte(static_cast<uint8>(x));
                source.at(offset + 1U) = asByte(static_cast<uint8>(y));
                source.at(offset + 2U) = asByte(0);
                source.at(offset + 3U) = asByte(255);
            }
        }

        auto const rect = PixelRect::create(1, 1, 2, 2);
        REQUIRE(rect.has_value());
        auto const cropped = cropBgra8(source, 3, 3, stride, *rect);
        REQUIRE(cropped.has_value());
        auto const expected = std::vector<std::byte>{
            asByte(1), asByte(1), asByte(0), asByte(255),
            asByte(2), asByte(1), asByte(0), asByte(255),
            asByte(1), asByte(2), asByte(0), asByte(255),
            asByte(2), asByte(2), asByte(0), asByte(255),
        };
        CHECK(*cropped == expected);
    }

    TEST_CASE("image BGRA8 crop rejects rows outside the source buffer")
    {
        auto const rect = PixelRect::create(0, 0, 2, 2);
        REQUIRE(rect.has_value());
        auto const cropped = cropBgra8(
            std::array{asByte(1), asByte(2), asByte(3), asByte(4)},
            2,
            2,
            4,
            *rect
        );
        REQUIRE_FALSE(cropped.has_value());
        test_image::requireErrorKind(
            cropped.error(),
            AutomationErrorKind::InvalidResource
        );
    }
}
