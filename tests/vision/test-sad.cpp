#include <vision/sad.hpp>
#include <vision/synthetic.hpp>

#include <core/error/contracts.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/safety/checked-access.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace
{
    [[nodiscard]]
    constexpr auto asByte(std::uint8_t value) noexcept -> std::byte
    {
        return std::byte{value};
    }

    class HashedSample final
    {
        std::uint32_t m_seed;

    public:
        constexpr explicit HashedSample(std::uint32_t seed) noexcept
            : m_seed{seed}
        {
        }

        [[nodiscard]]
        auto operator()(std::uint32_t x, std::uint32_t y) const noexcept -> std::uint8_t
        {
            return uf::hashedGray(m_seed, x, y);
        }
    };

    [[nodiscard]]
    auto hashed(std::uint32_t seed) noexcept -> HashedSample
    {
        return HashedSample{seed};
    }

    [[nodiscard]]
    auto grayOffset(
        std::size_t stride,
        std::uint32_t x,
        std::uint32_t y
    ) noexcept -> std::size_t
    {
        auto const xSize = uf::checkedCast<std::size_t>(x);
        auto const ySize = uf::checkedCast<std::size_t>(y);
        UF_CHECK(xSize.has_value());
        UF_CHECK(ySize.has_value());

        auto const rowOffset = uf::checkedMultiply(*ySize, stride);
        UF_CHECK(rowOffset.has_value());
        auto const offset = uf::checkedAdd(*rowOffset, *xSize);
        UF_CHECK(offset.has_value());
        return *offset;
    }

    [[nodiscard]]
    auto grayPixel(
        std::span<std::byte const> data,
        std::size_t stride,
        std::uint32_t x,
        std::uint32_t y
    ) noexcept -> std::uint32_t
    {
        return std::to_integer<std::uint32_t>(
            uf::checkedAt(data, grayOffset(stride, x, y))
        );
    }

    auto writeBgraPixel(
        std::vector<std::byte>& data,
        std::size_t offset,
        std::array<std::uint8_t, 4> pixel
    ) -> void
    {
        for (auto index = std::size_t{0}; index < pixel.size(); ++index)
        {
            auto const destination = uf::checkedAdd(offset, index);
            UF_CHECK(destination.has_value());
            uf::checkedAt(data, *destination) = asByte(
                uf::checkedAt(pixel, index)
            );
        }
    }

    template <typename Sample>
    [[nodiscard]]
    auto build(
        std::uint32_t width,
        std::uint32_t height,
        std::size_t stride,
        std::byte padding,
        Sample const& sample
    ) -> std::vector<std::byte>
    {
        auto const widthSize = uf::checkedCast<std::size_t>(width);
        auto const heightSize = uf::checkedCast<std::size_t>(height);
        UF_CHECK(widthSize.has_value());
        UF_CHECK(heightSize.has_value());
        UF_CHECK(stride >= *widthSize);

        auto const bufferLength = uf::checkedMultiply(stride, *heightSize);
        UF_CHECK(bufferLength.has_value());
        auto buffer = std::vector<std::byte>(
            *bufferLength,
            padding
        );
        for (auto y = std::uint32_t{0}; y < height; ++y)
        {
            for (auto x = std::uint32_t{0}; x < width; ++x)
            {
                auto const sampleValue = uf::checkedCast<std::uint8_t>(sample(x, y));
                UF_CHECK(sampleValue.has_value());
                uf::checkedAt(buffer, grayOffset(stride, x, y)) = asByte(*sampleValue);
            }
        }
        return buffer;
    }

    [[nodiscard]]
    auto pixelRect(
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t width,
        std::uint32_t height
    ) -> uf::PixelRect
    {
        auto const result = uf::PixelRect::create(x, y, width, height);
        REQUIRE(result.has_value());
        return *result;
    }

    [[nodiscard]]
    auto grayImage(
        std::vector<std::byte> const& data UF_LIFETIME_BOUND,
        std::uint32_t width,
        std::uint32_t height,
        std::size_t stride
    ) -> uf::GrayImage
    {
        auto const result = uf::GrayImage::create(
            std::span<std::byte const>{data},
            width,
            height,
            stride
        );
        REQUIRE(result.has_value());
        return *result;
    }

    auto requireErrorKind(
        uf::Error const& error,
        uf::AutomationErrorKind expected
    ) -> void
    {
        auto const kind = uf::automationErrorKind(error);
        REQUIRE(kind.has_value());
        CHECK(*kind == expected);
    }

    [[nodiscard]]
    auto bruteForce(
        uf::GrayImage const& haystack,
        std::span<std::byte const> haystackData,
        uf::GrayImage const& templateImage,
        std::span<std::byte const> templateData,
        uf::PixelRect roi
    ) -> std::optional<uf::SadMatch>
    {
        if (
            templateImage.width() > roi.width()
            || templateImage.height() > roi.height()
        )
        {
            return std::nullopt;
        }

        auto const lastX = uf::checkedSubtract(roi.right(), templateImage.width());
        auto const lastY = uf::checkedSubtract(roi.bottom(), templateImage.height());
        UF_CHECK(lastX.has_value());
        UF_CHECK(lastY.has_value());
        auto best = std::optional<uf::SadMatch>{};
        for (auto candidateY = roi.y(); candidateY <= *lastY; ++candidateY)
        {
            for (auto candidateX = roi.x(); candidateX <= *lastX; ++candidateX)
            {
                auto score = std::uint64_t{0};
                for (auto templateY = std::uint32_t{0}; templateY < templateImage.height(); ++templateY)
                {
                    for (auto templateX = std::uint32_t{0}; templateX < templateImage.width(); ++templateX)
                    {
                        auto const haystackX = uf::checkedAdd(
                            candidateX,
                            templateX
                        );
                        auto const haystackY = uf::checkedAdd(
                            candidateY,
                            templateY
                        );
                        UF_CHECK(haystackX.has_value());
                        UF_CHECK(haystackY.has_value());
                        auto const haystackPixel = grayPixel(
                            haystackData,
                            haystack.stride(),
                            *haystackX,
                            *haystackY
                        );
                        auto const templatePixel = grayPixel(
                            templateData,
                            templateImage.stride(),
                            templateX,
                            templateY
                        );
                        auto const difference = (
                            haystackPixel >= templatePixel
                                ? haystackPixel - templatePixel
                                : templatePixel - haystackPixel
                        );
                        score += difference;
                    }
                }

                if (!best || score < best->score())
                {
                    best.emplace(candidateX, candidateY, score);
                }
            }
        }

        return best;
    }
}

TEST_CASE("exact template hit scores zero")
{
    auto constexpr haystackWidth = std::uint32_t{96};
    auto constexpr haystackHeight = std::uint32_t{64};
    auto const background = hashed(1);
    auto const haystackData = build(
        haystackWidth,
        haystackHeight,
        haystackWidth,
        asByte(0),
        background
    );
    auto const haystack = grayImage(
        haystackData,
        haystackWidth,
        haystackHeight,
        haystackWidth
    );

    auto constexpr matchX = std::uint32_t{37};
    auto constexpr matchY = std::uint32_t{21};
    auto constexpr templateWidth = std::uint32_t{12};
    auto constexpr templateHeight = std::uint32_t{9};
    auto const templateData = build(
        templateWidth,
        templateHeight,
        templateWidth,
        asByte(0),
        [background](std::uint32_t x, std::uint32_t y) noexcept -> std::uint8_t
        {
            return background(matchX + x, matchY + y);
        }
    );
    auto const templateImage = grayImage(
        templateData,
        templateWidth,
        templateHeight,
        templateWidth
    );

    auto const result = uf::matchTemplateSad(
        haystack,
        templateImage,
        pixelRect(0, 0, haystackWidth, haystackHeight)
    );
    REQUIRE(result.has_value());
    REQUIRE(result->has_value());
    CHECK(**result == uf::SadMatch{matchX, matchY, 0});
}

TEST_CASE("template matching agrees with an exhaustive scan")
{
    auto constexpr haystackWidth = std::uint32_t{80};
    auto constexpr haystackHeight = std::uint32_t{60};
    auto constexpr templateWidth = std::uint32_t{10};
    auto constexpr templateHeight = std::uint32_t{8};

    auto const background = hashed(7);
    auto const haystackAData = build(
        haystackWidth,
        haystackHeight,
        haystackWidth,
        asByte(0),
        background
    );
    auto const haystackA = grayImage(
        haystackAData,
        haystackWidth,
        haystackHeight,
        haystackWidth
    );
    auto const templateAData = build(
        templateWidth,
        templateHeight,
        templateWidth,
        asByte(0),
        [background](std::uint32_t x, std::uint32_t y) noexcept -> std::uint8_t
        {
            return background(20 + x, 15 + y);
        }
    );
    auto const templateA = grayImage(
        templateAData,
        templateWidth,
        templateHeight,
        templateWidth
    );

    auto const templateBData = build(
        templateWidth,
        templateHeight,
        templateWidth,
        asByte(0),
        hashed(999)
    );
    auto const templateB = grayImage(
        templateBData,
        templateWidth,
        templateHeight,
        templateWidth
    );

    auto const smoothData = build(
        haystackWidth,
        haystackHeight,
        haystackWidth,
        asByte(0),
        [](std::uint32_t x, std::uint32_t y) noexcept -> std::uint32_t
        {
            return 100 + ((x ^ y) & 0x07);
        }
    );
    auto const smooth = grayImage(
        smoothData,
        haystackWidth,
        haystackHeight,
        haystackWidth
    );
    auto const contrastData = build(
        templateWidth,
        templateHeight,
        templateWidth,
        asByte(0),
        [](std::uint32_t x, std::uint32_t y) noexcept -> std::uint32_t
        {
            return (x + y) % 2 == 0 ? 0U : 255U;
        }
    );
    auto const contrast = grayImage(
        contrastData,
        templateWidth,
        templateHeight,
        templateWidth
    );

    auto const rois = std::array{
        pixelRect(0, 0, haystackWidth, haystackHeight),
        pixelRect(5, 4, 50, 40),
        pixelRect(0, 0, 79, 59),
    };
    auto const checkRegime = [rois](
        uf::GrayImage const& haystack,
        std::span<std::byte const> haystackData,
        uf::GrayImage const& templateImage,
        std::span<std::byte const> templateData
    ) -> void
    {
        for (auto const roi : rois)
        {
            auto const result = uf::matchTemplateSad(
                haystack,
                templateImage,
                roi
            );
            REQUIRE(result.has_value());
            CHECK(
                *result
                == bruteForce(
                    haystack,
                    haystackData,
                    templateImage,
                    templateData,
                    roi
                )
            );
        }
    };

    checkRegime(haystackA, haystackAData, templateA, templateAData);
    checkRegime(haystackA, haystackAData, templateB, templateBData);
    checkRegime(smooth, smoothData, contrast, contrastData);
}

TEST_CASE("ties resolve to earliest row major placement")
{
    auto constexpr haystackWidth = std::uint32_t{4};
    auto constexpr haystackHeight = std::uint32_t{4};
    auto const haystackData = build(
        haystackWidth,
        haystackHeight,
        haystackWidth,
        asByte(0),
        [](std::uint32_t x, std::uint32_t y) noexcept -> std::uint8_t
        {
            return (
                (x == 3 && y == 0)
                || (x == 0 && y == 3)
            ) ? 255 : 0;
        }
    );
    auto const templateData = build(
        1,
        1,
        1,
        asByte(0),
        [](std::uint32_t, std::uint32_t) noexcept -> std::uint8_t
        {
            return 255;
        }
    );

    auto const haystack = grayImage(
        haystackData,
        haystackWidth,
        haystackHeight,
        haystackWidth
    );
    auto const templateImage = grayImage(
        templateData,
        1,
        1,
        1
    );
    auto const result = uf::matchTemplateSad(
        haystack,
        templateImage,
        pixelRect(0, 0, haystackWidth, haystackHeight)
    );

    REQUIRE(result.has_value());
    REQUIRE(result->has_value());
    CHECK(**result == uf::SadMatch{3, 0, 0});
}

TEST_CASE("padded strides are not misread")
{
    auto constexpr haystackWidth = std::uint32_t{40};
    auto constexpr haystackHeight = std::uint32_t{30};
    auto constexpr haystackStride = std::size_t{64};
    auto const background = hashed(11);
    auto const haystackData = build(
        haystackWidth,
        haystackHeight,
        haystackStride,
        asByte(0xFF),
        background
    );
    auto const haystack = grayImage(
        haystackData,
        haystackWidth,
        haystackHeight,
        haystackStride
    );

    auto constexpr matchX = std::uint32_t{15};
    auto constexpr matchY = std::uint32_t{9};
    auto constexpr templateWidth = std::uint32_t{9};
    auto constexpr templateHeight = std::uint32_t{7};
    auto constexpr templateStride = std::size_t{14};
    auto const templateData = build(
        templateWidth,
        templateHeight,
        templateStride,
        asByte(0xFF),
        [background](std::uint32_t x, std::uint32_t y) noexcept -> std::uint8_t
        {
            return background(matchX + x, matchY + y);
        }
    );
    auto const templateImage = grayImage(
        templateData,
        templateWidth,
        templateHeight,
        templateStride
    );

    auto const fullRoi = pixelRect(0, 0, haystackWidth, haystackHeight);
    auto const result = uf::matchTemplateSad(haystack, templateImage, fullRoi);
    REQUIRE(result.has_value());
    REQUIRE(result->has_value());
    CHECK(**result == uf::SadMatch{matchX, matchY, 0});
}

TEST_CASE("template fit uses the exact roi boundary")
{
    auto constexpr haystackWidth = std::uint32_t{3};
    auto constexpr haystackHeight = std::uint32_t{3};
    auto const haystackData = build(
        haystackWidth,
        haystackHeight,
        haystackWidth,
        asByte(0),
        [](std::uint32_t x, std::uint32_t y) noexcept -> std::uint32_t
        {
            return y * 3 + x;
        }
    );
    auto const templateData = build(
        2,
        2,
        2,
        asByte(0),
        [](std::uint32_t x, std::uint32_t y) noexcept -> std::uint32_t
        {
            return (y + 1) * 3 + x + 1;
        }
    );
    auto const haystack = grayImage(
        haystackData,
        haystackWidth,
        haystackHeight,
        haystackWidth
    );
    auto const templateImage = grayImage(templateData, 2, 2, 2);

    auto const exactFit = uf::matchTemplateSad(
        haystack,
        templateImage,
        pixelRect(1, 1, 2, 2)
    );
    REQUIRE(exactFit.has_value());
    REQUIRE(exactFit->has_value());
    CHECK(**exactFit == uf::SadMatch{1, 1, 0});

    auto const rois = std::array{
        pixelRect(2, 1, 1, 2),
        pixelRect(1, 2, 2, 1),
    };
    for (auto const roi : rois)
    {
        auto const result = uf::matchTemplateSad(haystack, templateImage, roi);
        REQUIRE(result.has_value());
        CHECK_FALSE(result->has_value());
    }
}

TEST_CASE("roi outside haystack is rejected")
{
    auto constexpr haystackWidth = std::uint32_t{32};
    auto constexpr haystackHeight = std::uint32_t{32};
    auto const haystackData = build(
        haystackWidth,
        haystackHeight,
        haystackWidth,
        asByte(0),
        hashed(5)
    );
    auto const templateData = build(8, 8, 8, asByte(0), hashed(6));
    auto const haystack = grayImage(
        haystackData,
        haystackWidth,
        haystackHeight,
        haystackWidth
    );
    auto const templateImage = grayImage(templateData, 8, 8, 8);

    auto const rois = std::array{
        pixelRect(32, 0, 1, 1),
        pixelRect(0, 32, 1, 1),
        pixelRect(0, 0, 33, 32),
        pixelRect(0, 0, 32, 33),
    };
    for (auto const roi : rois)
    {
        auto const result = uf::matchTemplateSad(haystack, templateImage, roi);
        REQUIRE_FALSE(result.has_value());
        requireErrorKind(result.error(), uf::AutomationErrorKind::ActionRejected);
    }
}

TEST_CASE("invalid gray image is rejected")
{
    struct InvalidCase final
    {
        std::size_t m_length;
        std::uint32_t m_width;
        std::uint32_t m_height;
        std::size_t m_stride;
    };

    auto const cases = std::array{
        InvalidCase{16, 0, 2, 8},
        InvalidCase{16, 4, 0, 8},
        InvalidCase{16, 4, 2, 3},
        InvalidCase{10, 4, 4, 4},
    };
    for (auto const& testCase : cases)
    {
        auto const data = std::vector<std::byte>(testCase.m_length);
        auto const result = uf::GrayImage::create(
            std::span<std::byte const>{data},
            testCase.m_width,
            testCase.m_height,
            testCase.m_stride
        );
        REQUIRE_FALSE(result.has_value());
        requireErrorKind(result.error(), uf::AutomationErrorKind::InternalInvariant);
    }
}

TEST_CASE("bgra8 to gray8 uses BT.601 weights")
{
    struct ConversionCase final
    {
        std::array<std::byte, 4> m_bgra;
        std::uint8_t m_expected;
    };

    auto const cases = std::array{
        ConversionCase{{asByte(0), asByte(0), asByte(255), asByte(255)}, 76},
        ConversionCase{{asByte(0), asByte(255), asByte(0), asByte(255)}, 149},
        ConversionCase{{asByte(255), asByte(0), asByte(0), asByte(255)}, 28},
        ConversionCase{{asByte(255), asByte(255), asByte(255), asByte(255)}, 255},
        ConversionCase{{asByte(0), asByte(0), asByte(0), asByte(255)}, 0},
        ConversionCase{{asByte(17), asByte(89), asByte(200), asByte(255)}, 114},
    };
    for (auto const& testCase : cases)
    {
        auto const result = uf::bgra8ToGray8(testCase.m_bgra, 1, 1, 4);
        REQUIRE(result.has_value());
        REQUIRE(result->size() == 1);
        CHECK(std::to_integer<std::uint8_t>(result->front()) == testCase.m_expected);
    }
}

TEST_CASE("bgra8 to gray8 ignores alpha")
{
    auto const transparent = std::array{
        asByte(17),
        asByte(89),
        asByte(200),
        asByte(0),
    };
    auto const opaque = std::array{
        asByte(17),
        asByte(89),
        asByte(200),
        asByte(255),
    };

    auto const transparentGray = uf::bgra8ToGray8(transparent, 1, 1, 4);
    auto const opaqueGray = uf::bgra8ToGray8(opaque, 1, 1, 4);
    REQUIRE(transparentGray.has_value());
    REQUIRE(opaqueGray.has_value());
    CHECK(*transparentGray == *opaqueGray);
}

TEST_CASE("bgra8 to gray8 honors stride and is tightly packed")
{
    auto constexpr stride = std::size_t{12};
    auto const dataLength = uf::checkedMultiply(stride, std::size_t{2});
    UF_CHECK(dataLength.has_value());
    auto data = std::vector<std::byte>(*dataLength, asByte(0xEE));
    writeBgraPixel(data, 0, {0, 0, 255, 255});
    writeBgraPixel(data, 4, {0, 255, 0, 255});
    writeBgraPixel(data, stride, {255, 0, 0, 255});
    auto const finalPixelOffset = uf::checkedAdd(stride, std::size_t{4});
    UF_CHECK(finalPixelOffset.has_value());
    writeBgraPixel(data, *finalPixelOffset, {255, 255, 255, 255});

    auto const result = uf::bgra8ToGray8(data, 2, 2, stride);
    REQUIRE(result.has_value());
    auto const expected = std::vector<std::byte>{
        asByte(76),
        asByte(149),
        asByte(28),
        asByte(255),
    };
    CHECK(*result == expected);
}

TEST_CASE("bgra8 to gray8 rejects bad geometry")
{
    struct InvalidCase final
    {
        std::size_t m_length;
        std::uint32_t m_width;
        std::uint32_t m_height;
        std::size_t m_stride;
    };

    auto const cases = std::array{
        InvalidCase{16, 0, 1, 4},
        InvalidCase{16, 1, 0, 4},
        InvalidCase{16, 2, 1, 4},
        InvalidCase{4, 2, 1, 8},
    };
    for (auto const& testCase : cases)
    {
        auto const data = std::vector<std::byte>(testCase.m_length);
        auto const result = uf::bgra8ToGray8(
            data,
            testCase.m_width,
            testCase.m_height,
            testCase.m_stride
        );
        REQUIRE_FALSE(result.has_value());
        requireErrorKind(result.error(), uf::AutomationErrorKind::InternalInvariant);
    }
}
