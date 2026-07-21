#include <vision/sad.hpp>
#include <vision/synthetic.hpp>

#include <core/error/contracts.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/safety/checked-access.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace uf
{
    namespace
    {
        [[nodiscard]]
        constexpr auto asByte(uint8 value) noexcept -> std::byte
        {
            return std::byte{value};
        }

        class HashedSample final
        {
            uint32 m_seed;

        public:
            constexpr explicit HashedSample(uint32 seed) noexcept
                : m_seed{seed}
            {
            }

            [[nodiscard]]
            auto operator()(uint32 x, uint32 y) const noexcept -> uint8
            {
                return hashedGray(m_seed, x, y);
            }
        };

        [[nodiscard]]
        auto hashed(uint32 seed) noexcept -> HashedSample
        {
            return HashedSample{seed};
        }

        [[nodiscard]]
        auto grayOffset(
            std::size_t stride,
            uint32 x,
            uint32 y
        ) noexcept -> std::size_t
        {
            auto const xSize = checkedCast<std::size_t>(x);
            auto const ySize = checkedCast<std::size_t>(y);
            UF_CHECK(xSize.has_value());
            UF_CHECK(ySize.has_value());

            auto const rowOffset = checkedMultiply(*ySize, stride);
            UF_CHECK(rowOffset.has_value());
            auto const offset = checkedAdd(*rowOffset, *xSize);
            UF_CHECK(offset.has_value());
            return *offset;
        }

        [[nodiscard]]
        auto grayPixel(
            std::span<std::byte const> data,
            std::size_t stride,
            uint32 x,
            uint32 y
        ) noexcept -> uint32
        {
            return std::to_integer<uint32>(
                checkedAt(data, grayOffset(stride, x, y))
            );
        }

        auto writeBgraPixel(
            std::vector<std::byte>& data,
            std::size_t offset,
            std::array<uint8, 4> pixel
        ) -> void
        {
            for (auto index = std::size_t{0}; index < pixel.size(); ++index)
            {
                auto const destination = checkedAdd(offset, index);
                UF_CHECK(destination.has_value());
                checkedAt(data, *destination) = asByte(
                    checkedAt(pixel, index)
                );
            }
        }

        template <typename Sample>
        [[nodiscard]]
        auto build(
            uint32 width,
            uint32 height,
            std::size_t stride,
            std::byte padding,
            Sample const& sample
        ) -> std::vector<std::byte>
        {
            auto const widthSize = checkedCast<std::size_t>(width);
            auto const heightSize = checkedCast<std::size_t>(height);
            UF_CHECK(widthSize.has_value());
            UF_CHECK(heightSize.has_value());
            UF_CHECK(stride >= *widthSize);

            auto const bufferLength = checkedMultiply(stride, *heightSize);
            UF_CHECK(bufferLength.has_value());
            auto buffer = std::vector<std::byte>(
                *bufferLength,
                padding
            );
            for (auto y = uint32{0}; y < height; ++y)
            {
                for (auto x = uint32{0}; x < width; ++x)
                {
                    auto const sampleValue = checkedCast<uint8>(sample(x, y));
                    UF_CHECK(sampleValue.has_value());
                    checkedAt(buffer, grayOffset(stride, x, y)) = asByte(*sampleValue);
                }
            }
            return buffer;
        }

        [[nodiscard]]
        auto pixelRect(
            uint32 x,
            uint32 y,
            uint32 width,
            uint32 height
        ) -> PixelRect
        {
            auto const result = PixelRect::create(x, y, width, height);
            REQUIRE(result.has_value());
            return *result;
        }

        [[nodiscard]]
        auto grayImage(
            std::vector<std::byte> const& data UF_LIFETIME_BOUND,
            uint32 width,
            uint32 height,
            std::size_t stride
        ) -> GrayImage
        {
            auto const result = GrayImage::create(
                std::span<std::byte const>{data},
                width,
                height,
                stride
            );
            REQUIRE(result.has_value());
            return *result;
        }

        auto requireErrorKind(
            Error const& error,
            AutomationErrorKind expected
        ) -> void
        {
            auto const kind = automationErrorKind(error);
            REQUIRE(kind.has_value());
            CHECK(kind == expected);
        }

        [[nodiscard]]
        auto bruteForce(
            GrayImage const& haystack,
            std::span<std::byte const> haystackData,
            GrayImage const& templateImage,
            std::span<std::byte const> templateData,
            PixelRect roi
        ) -> std::optional<SadMatch>
        {
            if (
                templateImage.width() > roi.width()
                || templateImage.height() > roi.height()
            )
            {
                return std::nullopt;
            }

            auto const lastX = checkedSubtract(roi.right(), templateImage.width());
            auto const lastY = checkedSubtract(roi.bottom(), templateImage.height());
            UF_CHECK(lastX.has_value());
            UF_CHECK(lastY.has_value());
            auto best = std::optional<SadMatch>{};
            for (auto candidateY = roi.y(); candidateY <= *lastY; ++candidateY)
            {
                for (auto candidateX = roi.x(); candidateX <= *lastX; ++candidateX)
                {
                    auto score = uint64{0};
                    for (auto templateY = uint32{0}; templateY < templateImage.height(); ++templateY)
                    {
                        for (auto templateX = uint32{0}; templateX < templateImage.width(); ++templateX)
                        {
                            auto const haystackX = checkedAdd(
                                candidateX,
                                templateX
                            );
                            auto const haystackY = checkedAdd(
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
        auto constexpr haystackWidth = uint32{96};
        auto constexpr haystackHeight = uint32{64};
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

        auto constexpr matchX = uint32{37};
        auto constexpr matchY = uint32{21};
        auto constexpr templateWidth = uint32{12};
        auto constexpr templateHeight = uint32{9};
        auto const templateData = build(
            templateWidth,
            templateHeight,
            templateWidth,
            asByte(0),
            [background](uint32 x, uint32 y) noexcept -> uint8
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

        auto const result = matchTemplateSad(
            haystack,
            templateImage,
            pixelRect(0, 0, haystackWidth, haystackHeight)
        );
        REQUIRE(result.has_value());
        CHECK(*result == std::optional{SadMatch{matchX, matchY, 0}});
    }

    TEST_CASE("template matching enforces exact pixel comparison budgets")
    {
        auto const haystackData = std::vector<std::byte>{
            asByte(0),
            asByte(1),
        };
        auto const templateData = std::vector<std::byte>{asByte(255)};
        auto const haystack = grayImage(haystackData, 2, 1, 2);
        auto const templateImage = grayImage(templateData, 1, 1, 1);
        auto const roi = pixelRect(0, 0, 2, 1);
        auto pollCount = uint32{0};
        auto const continueSearch = SadSearchPoll{
            [&pollCount]() noexcept -> SadSearchControl
            {
                ++pollCount;
                return SadSearchControl::Continue;
            }
        };

        auto const zeroBudget = matchTemplateSad(
            haystack,
            templateImage,
            roi,
            0,
            continueSearch
        );
        REQUIRE(zeroBudget.has_value());
        CHECK(
            std::get<SadSearchStopReason>(*zeroBudget)
            == SadSearchStopReason::ComparisonBudgetExhausted
        );
        CHECK(pollCount == 0);

        auto const oneComparison = matchTemplateSad(
            haystack,
            templateImage,
            roi,
            1,
            continueSearch
        );
        REQUIRE(oneComparison.has_value());
        CHECK(
            std::get<SadSearchStopReason>(*oneComparison)
            == SadSearchStopReason::ComparisonBudgetExhausted
        );
        CHECK(pollCount == 1);

        auto const exactBudget = matchTemplateSad(
            haystack,
            templateImage,
            roi,
            2,
            continueSearch
        );
        REQUIRE(exactBudget.has_value());
        CHECK(
            std::get<std::optional<SadMatch>>(*exactBudget)
            == std::optional{SadMatch{1, 0, 254}}
        );
    }

    TEST_CASE("template matching maps synchronous poll interruptions")
    {
        struct InterruptionCase final
        {
            SadSearchControl m_control;
            SadSearchStopReason m_expected;
        };

        auto const data = std::vector<std::byte>{asByte(0)};
        auto const image = grayImage(data, 1, 1, 1);
        auto const roi = pixelRect(0, 0, 1, 1);
        auto const cases = std::array{
            InterruptionCase{
                SadSearchControl::Cancelled,
                SadSearchStopReason::Cancelled
            },
            InterruptionCase{
                SadSearchControl::TimedOut,
                SadSearchStopReason::TimedOut
            },
        };
        for (auto const& testCase : cases)
        {
            auto const poll = SadSearchPoll{
                [control = testCase.m_control]() noexcept -> SadSearchControl
                {
                    return control;
                }
            };
            auto const result = matchTemplateSad(
                image,
                image,
                roi,
                1,
                poll
            );
            REQUIRE(result.has_value());
            CHECK(std::get<SadSearchStopReason>(*result) == testCase.m_expected);
        }
    }

    TEST_CASE("template matching polls within the documented comparison interval")
    {
        auto constexpr haystackWidth = uint32{4097};
        static_assert(
            haystackWidth
            == g_sadSearchPollIntervalComparisons + uint64{1}
        );
        auto const haystackData = std::vector<std::byte>(
            haystackWidth,
            asByte(0)
        );
        auto const templateData = std::vector<std::byte>{asByte(255)};
        auto const haystack = grayImage(
            haystackData,
            haystackWidth,
            1,
            haystackWidth
        );
        auto const templateImage = grayImage(templateData, 1, 1, 1);
        auto pollCount = uint32{0};
        auto const cancelOnSecondPoll = SadSearchPoll{
            [&pollCount]() noexcept -> SadSearchControl
            {
                ++pollCount;
                if (pollCount == 2)
                {
                    return SadSearchControl::Cancelled;
                }
                return SadSearchControl::Continue;
            }
        };

        auto const result = matchTemplateSad(
            haystack,
            templateImage,
            pixelRect(0, 0, haystackWidth, 1),
            uint64{haystackWidth},
            cancelOnSecondPoll
        );

        REQUIRE(result.has_value());
        CHECK(
            std::get<SadSearchStopReason>(*result)
            == SadSearchStopReason::Cancelled
        );
        CHECK(pollCount == 2);
    }

    TEST_CASE("template matching agrees with an exhaustive scan")
    {
        auto constexpr haystackWidth = uint32{80};
        auto constexpr haystackHeight = uint32{60};
        auto constexpr templateWidth = uint32{10};
        auto constexpr templateHeight = uint32{8};

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
            [background](uint32 x, uint32 y) noexcept -> uint8
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
            [](uint32 x, uint32 y) noexcept -> uint32
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
            [](uint32 x, uint32 y) noexcept -> uint32
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
            GrayImage const& haystack,
            std::span<std::byte const> haystackData,
            GrayImage const& templateImage,
            std::span<std::byte const> templateData
        ) -> void
        {
            for (auto const roi : rois)
            {
                auto const result = matchTemplateSad(
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
        auto constexpr haystackWidth = uint32{4};
        auto constexpr haystackHeight = uint32{4};
        auto const haystackData = build(
            haystackWidth,
            haystackHeight,
            haystackWidth,
            asByte(0),
            [](uint32 x, uint32 y) noexcept -> uint8
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
            [](uint32, uint32) noexcept -> uint8
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
        auto const result = matchTemplateSad(
            haystack,
            templateImage,
            pixelRect(0, 0, haystackWidth, haystackHeight)
        );

        REQUIRE(result.has_value());
        CHECK(*result == std::optional{SadMatch{3, 0, 0}});
    }

    TEST_CASE("padded strides are not misread")
    {
        auto constexpr haystackWidth = uint32{40};
        auto constexpr haystackHeight = uint32{30};
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

        auto constexpr matchX = uint32{15};
        auto constexpr matchY = uint32{9};
        auto constexpr templateWidth = uint32{9};
        auto constexpr templateHeight = uint32{7};
        auto constexpr templateStride = std::size_t{14};
        auto const templateData = build(
            templateWidth,
            templateHeight,
            templateStride,
            asByte(0xFF),
            [background](uint32 x, uint32 y) noexcept -> uint8
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
        auto const result = matchTemplateSad(haystack, templateImage, fullRoi);
        REQUIRE(result.has_value());
        CHECK(*result == std::optional{SadMatch{matchX, matchY, 0}});
    }

    TEST_CASE("template fit uses the exact roi boundary")
    {
        auto constexpr haystackWidth = uint32{3};
        auto constexpr haystackHeight = uint32{3};
        auto const haystackData = build(
            haystackWidth,
            haystackHeight,
            haystackWidth,
            asByte(0),
            [](uint32 x, uint32 y) noexcept -> uint32
            {
                return y * 3 + x;
            }
        );
        auto const templateData = build(
            2,
            2,
            2,
            asByte(0),
            [](uint32 x, uint32 y) noexcept -> uint32
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

        auto const exactFit = matchTemplateSad(
            haystack,
            templateImage,
            pixelRect(1, 1, 2, 2)
        );
        REQUIRE(exactFit.has_value());
        CHECK(*exactFit == std::optional{SadMatch{1, 1, 0}});

        auto const rois = std::array{
            pixelRect(2, 1, 1, 2),
            pixelRect(1, 2, 2, 1),
        };
        for (auto const roi : rois)
        {
            auto const result = matchTemplateSad(haystack, templateImage, roi);
            REQUIRE(result.has_value());
            CHECK_FALSE(result->has_value());
        }
    }

    TEST_CASE("roi outside haystack is rejected")
    {
        auto constexpr haystackWidth = uint32{32};
        auto constexpr haystackHeight = uint32{32};
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
            auto const result = matchTemplateSad(haystack, templateImage, roi);
            REQUIRE_FALSE(result.has_value());
            requireErrorKind(result.error(), AutomationErrorKind::ActionRejected);
        }
    }

    TEST_CASE("invalid gray image is rejected")
    {
        struct InvalidCase final
        {
            std::size_t m_length;
            uint32 m_width;
            uint32 m_height;
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
            auto const result = GrayImage::create(
                std::span<std::byte const>{data},
                testCase.m_width,
                testCase.m_height,
                testCase.m_stride
            );
            REQUIRE_FALSE(result.has_value());
            requireErrorKind(result.error(), AutomationErrorKind::InternalInvariant);
        }
    }

    TEST_CASE("bgra8 to gray8 uses BT.601 weights")
    {
        struct ConversionCase final
        {
            std::array<std::byte, 4> m_bgra;
            uint8 m_expected;
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
            auto const result = bgra8ToGray8(testCase.m_bgra, 1, 1, 4);
            REQUIRE(result.has_value());
            REQUIRE(result->size() == 1);
            CHECK(std::to_integer<uint8>(result->front()) == testCase.m_expected);
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

        auto const transparentGray = bgra8ToGray8(transparent, 1, 1, 4);
        auto const opaqueGray = bgra8ToGray8(opaque, 1, 1, 4);
        REQUIRE(transparentGray.has_value());
        REQUIRE(opaqueGray.has_value());
        CHECK(*transparentGray == *opaqueGray);
    }

    TEST_CASE("bgra8 to gray8 honors stride and is tightly packed")
    {
        auto constexpr stride = std::size_t{12};
        auto const dataLength = checkedMultiply(stride, std::size_t{2});
        UF_CHECK(dataLength.has_value());
        auto data = std::vector<std::byte>(*dataLength, asByte(0xEE));
        writeBgraPixel(data, 0, {0, 0, 255, 255});
        writeBgraPixel(data, 4, {0, 255, 0, 255});
        writeBgraPixel(data, stride, {255, 0, 0, 255});
        auto const finalPixelOffset = checkedAdd(stride, std::size_t{4});
        UF_CHECK(finalPixelOffset.has_value());
        writeBgraPixel(data, *finalPixelOffset, {255, 255, 255, 255});

        auto const result = bgra8ToGray8(data, 2, 2, stride);
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
            uint32 m_width;
            uint32 m_height;
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
            auto const result = bgra8ToGray8(
                data,
                testCase.m_width,
                testCase.m_height,
                testCase.m_stride
            );
            REQUIRE_FALSE(result.has_value());
            requireErrorKind(result.error(), AutomationErrorKind::InternalInvariant);
        }
    }
}
