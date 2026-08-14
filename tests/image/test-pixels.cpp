#include "test-helpers.hpp"

#include <image/pixels.hpp>
#include <image/template-cut.hpp>

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

    TEST_CASE("template cut weights a stable mark above changing ground")
    {
        auto firstPixels  = std::vector<std::byte>{};
        auto secondPixels = std::vector<std::byte>{};
        firstPixels.reserve(std::size_t{3} * 3U * 4U);
        secondPixels.reserve(std::size_t{3} * 3U * 4U);
        for (auto y = uint32{0}; y < 3U; ++y)
        {
            for (auto x = uint32{0}; x < 3U; ++x)
            {
                auto const mark = x == 1U || y == 1U;
                auto const edge = !mark && x == y;
                auto const first  = mark ? uint8{180} : edge ? uint8{100} : uint8{20};
                auto const second = mark ? uint8{180} : edge ? uint8{200} : uint8{220};
                for (auto channel = uint32{0}; channel < 3U; ++channel)
                {
                    firstPixels.emplace_back(asByte(first));
                    secondPixels.emplace_back(asByte(second));
                }
                firstPixels.emplace_back(asByte(7));
                secondPixels.emplace_back(asByte(9));
            }
        }

        auto const rect = PixelRect::create(0, 0, 3, 3);
        REQUIRE(rect.has_value());
        auto const sources = std::array{
            RgbaImage{.width = 3, .height = 3, .pixels = std::move(firstPixels)},
            RgbaImage{.width = 3, .height = 3, .pixels = std::move(secondPixels)},
        };
        auto const cut = cutRgba8Template(sources, *rect);
        REQUIRE(cut.has_value());
        CHECK(cut->alphaDerivation == TemplateAlphaDerivation::ObservedSpread);

        auto const alphaAt = [&cut](std::size_t pixel) -> uint8
        {
            return std::to_integer<uint8>(cut->image.pixels.at(pixel * 4U + 3U));
        };
        CHECK(alphaAt(4) == 255U);
        CHECK(alphaAt(0) == 128U);
        CHECK(alphaAt(2) == 0U);
        CHECK(alphaAt(4) > alphaAt(2));
    }

    TEST_CASE("template cut denies full weight where every pixel moved")
    {
        // Equal channels make Gray8 equal to the channel value, so the three
        // pixels below hold observed spreads of 60, 120 and 180. None of them
        // held still, so none of them may carry a mark's full weight.
        auto const rect = PixelRect::create(0, 0, 3, 1);
        REQUIRE(rect.has_value());
        auto const sources = std::array{
            RgbaImage{
                .width  = 3,
                .height = 1,
                .pixels = {
                    asByte(60), asByte(60), asByte(60), asByte(0),
                    asByte(60), asByte(60), asByte(60), asByte(0),
                    asByte(60), asByte(60), asByte(60), asByte(0),
                },
            },
            RgbaImage{
                .width  = 3,
                .height = 1,
                .pixels = {
                    asByte(120), asByte(120), asByte(120), asByte(0),
                    asByte(180), asByte(180), asByte(180), asByte(0),
                    asByte(240), asByte(240), asByte(240), asByte(0),
                },
            },
        };

        auto const cut = cutRgba8Template(sources, *rect);
        REQUIRE(cut.has_value());
        CHECK(cut->alphaDerivation == TemplateAlphaDerivation::ObservedSpread);

        auto const alphaAt = [&cut](std::size_t pixel) -> uint8
        {
            return std::to_integer<uint8>(cut->image.pixels.at(pixel * 4U + 3U));
        };
        CHECK(alphaAt(0) == 170U);
        CHECK(alphaAt(1) == 85U);
        CHECK(alphaAt(2) == 0U);
    }

    TEST_CASE("one-source template cut is explicitly fully opaque")
    {
        auto const rect = PixelRect::create(0, 0, 2, 1);
        REQUIRE(rect.has_value());
        auto const sources = std::array{
            RgbaImage{
                .width  = 2,
                .height = 1,
                .pixels = {
                    asByte(10), asByte(20), asByte(30), asByte(0),
                    asByte(40), asByte(50), asByte(60), asByte(70),
                },
            },
        };

        auto const cut = cutRgba8Template(sources, *rect);
        REQUIRE(cut.has_value());
        CHECK(
            cut->alphaDerivation
            == TemplateAlphaDerivation::SingleSourceOpaque
        );
        CHECK(cut->image.pixels.at(3) == asByte(255));
        CHECK(cut->image.pixels.at(7) == asByte(255));
    }

    TEST_CASE("template cut refuses a rectangle outside its source")
    {
        auto const rect = PixelRect::create(1, 1, 2, 2);
        REQUIRE(rect.has_value());
        auto const sources = std::array{
            RgbaImage{
                .width  = 2,
                .height = 2,
                .pixels = std::vector<std::byte>(std::size_t{2} * 2U * 4U),
            },
        };

        auto const cut = cutRgba8Template(sources, *rect);
        REQUIRE_FALSE(cut.has_value());
        test_image::requireErrorKind(
            cut.error(),
            AutomationErrorKind::ActionRejected
        );
    }
}
