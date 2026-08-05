#include "label-fixture.hpp"

#include <vision/bgra-image.hpp>
#include <vision/frame-analysis.hpp>
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
#include <limits>
#include <optional>
#include <span>
#include <string_view>
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

        // The grey level at which the label glyphs in the screenshot fixture
        // stop being antialiased edge and start being the UI's own stroke.
        constexpr auto k_nearWhiteGray = uint32{230};

        [[nodiscard]]
        auto decodeHex(std::string_view hex) -> std::vector<std::byte>
        {
            constexpr auto digits = std::string_view{"0123456789abcdef"};

            REQUIRE(hex.size() % 2 == 0);
            auto bytes = std::vector<std::byte>{};
            bytes.reserve(hex.size() / 2);
            for (auto index = std::size_t{0}; index < hex.size(); index += 2)
            {
                auto const high = digits.find(checkedAt(hex, index));
                auto const low  = digits.find(checkedAt(hex, index + 1));
                REQUIRE(high != std::string_view::npos);
                REQUIRE(low != std::string_view::npos);
                auto const value = checkedCast<uint8>(high * 16U + low);
                REQUIRE(value.has_value());
                bytes.emplace_back(asByte(*value));
            }
            return bytes;
        }

        [[nodiscard]]
        auto uniformMask(
            uint32 width,
            uint32 height,
            uint8 weight
        ) -> std::vector<std::byte>
        {
            return build(
                width,
                height,
                width,
                asByte(0),
                [weight](uint32, uint32) noexcept -> uint8
                {
                    return weight;
                }
            );
        }

        // Keeps the glyph strokes and drops everything the artwork owns, which
        // is the mask a template author would paint into the alpha channel.
        [[nodiscard]]
        auto nearWhiteMask(
            std::vector<std::byte> const& gray
        ) -> std::vector<std::byte>
        {
            auto mask = std::vector<std::byte>{};
            mask.reserve(gray.size());
            for (auto const value : gray)
            {
                mask.emplace_back(
                    std::to_integer<uint32>(value) >= k_nearWhiteGray
                        ? asByte(255)
                        : asByte(0)
                );
            }
            return mask;
        }

        [[nodiscard]]
        auto maskedPixelCount(std::vector<std::byte> const& mask) -> std::size_t
        {
            auto count = std::size_t{0};
            for (auto const value : mask)
            {
                if (value != asByte(0))
                {
                    ++count;
                }
            }
            return count;
        }

        [[nodiscard]]
        auto matchScore(Result<std::optional<SadMatch>> const& result) -> uint64
        {
            REQUIRE(result.has_value());
            REQUIRE(result->has_value());
            return (*result)->score();
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
            std::get<SadSearchStopReason>(zeroBudget->outcome)
            == SadSearchStopReason::ComparisonBudgetExhausted
        );
        CHECK(zeroBudget->completedPixelComparisons == 0);
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
            std::get<SadSearchStopReason>(oneComparison->outcome)
            == SadSearchStopReason::ComparisonBudgetExhausted
        );
        CHECK(oneComparison->completedPixelComparisons == 1);
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
            std::get<std::optional<SadMatch>>(exactBudget->outcome)
            == std::optional{SadMatch{1, 0, 254}}
        );
        CHECK(exactBudget->completedPixelComparisons == 2);
    }

    TEST_CASE("bounded exact match reports every comparison before early return")
    {
        auto const haystackData = std::vector<std::byte>{
            asByte(1),
            asByte(2),
            asByte(3),
        };
        auto const templateData  = std::vector<std::byte>{asByte(2)};
        auto const haystack      = grayImage(haystackData, 3, 1, 3);
        auto const templateImage = grayImage(templateData, 1, 1, 1);
        auto const roi           = pixelRect(0, 0, 3, 1);
        auto pollCount           = uint32{0};
        auto const poll = SadSearchPoll{
            [&pollCount]() noexcept -> SadSearchControl
            {
                ++pollCount;
                return SadSearchControl::Continue;
            }
        };

        auto const result = matchTemplateSad(
            haystack,
            templateImage,
            roi,
            3,
            poll
        );
        REQUIRE(result.has_value());
        CHECK(
            std::get<std::optional<SadMatch>>(result->outcome)
            == std::optional{SadMatch{1, 0, 0}}
        );
        CHECK(result->completedPixelComparisons == 2);
        CHECK(pollCount == 1);
    }

    TEST_CASE("template matching maps synchronous poll interruptions")
    {
        struct InterruptionCase final
        {
            SadSearchControl    control{};
            SadSearchStopReason expected{};
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
                [control = testCase.control]() noexcept -> SadSearchControl
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
            CHECK(
                std::get<SadSearchStopReason>(result->outcome)
                == testCase.expected
            );
            CHECK(result->completedPixelComparisons == 0);
        }
    }

    TEST_CASE("template matching polls within the documented comparison interval")
    {
        auto constexpr haystackWidth = uint32{4097};
        static_assert(
            haystackWidth
            == k_sadSearchPollIntervalComparisons + uint64{1}
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
            std::get<SadSearchStopReason>(result->outcome)
            == SadSearchStopReason::Cancelled
        );
        CHECK(
            result->completedPixelComparisons
            == k_sadSearchPollIntervalComparisons
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
            std::size_t length{};
            uint32 width{};
            uint32 height{};
            std::size_t stride{};
        };

        auto const cases = std::array{
            InvalidCase{16, 0, 2, 8},
            InvalidCase{16, 4, 0, 8},
            InvalidCase{16, 4, 2, 3},
            InvalidCase{10, 4, 4, 4},
        };
        for (auto const& testCase : cases)
        {
            auto const data = std::vector<std::byte>(testCase.length);
            auto const result = GrayImage::create(
                std::span<std::byte const>{data},
                testCase.width,
                testCase.height,
                testCase.stride
            );
            REQUIRE_FALSE(result.has_value());
            requireErrorKind(result.error(), AutomationErrorKind::InternalInvariant);
        }
    }

    TEST_CASE("bgra8 to gray8 uses BT.601 weights")
    {
        struct ConversionCase final
        {
            std::array<std::byte, 4> bgra{};
            uint8 expected{};
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
            auto const result = bgra8ToGray8(testCase.bgra, 1, 1, 4);
            REQUIRE(result.has_value());
            REQUIRE(result->size() == 1);
            CHECK(std::to_integer<uint8>(result->front()) == testCase.expected);
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
            std::size_t length{};
            uint32 width{};
            uint32 height{};
            std::size_t stride{};
        };

        auto const cases = std::array{
            InvalidCase{16, 0, 1, 4},
            InvalidCase{16, 1, 0, 4},
            InvalidCase{16, 2, 1, 4},
            InvalidCase{4, 2, 1, 8},
        };
        for (auto const& testCase : cases)
        {
            auto const data = std::vector<std::byte>(testCase.length);
            auto const result = bgra8ToGray8(
                data,
                testCase.width,
                testCase.height,
                testCase.stride
            );
            REQUIRE_FALSE(result.has_value());
            requireErrorKind(result.error(), AutomationErrorKind::InternalInvariant);
        }
    }

    TEST_CASE("bgra8 to alpha8 lifts the alpha channel into a gray8 plane")
    {
        auto constexpr stride = std::size_t{12};
        auto const dataLength = checkedMultiply(stride, std::size_t{2});
        UF_CHECK(dataLength.has_value());
        auto data = std::vector<std::byte>(*dataLength, asByte(0xEE));
        writeBgraPixel(data, 0, {1, 2, 3, 0});
        writeBgraPixel(data, 4, {4, 5, 6, 128});
        writeBgraPixel(data, stride, {7, 8, 9, 254});
        auto const finalPixelOffset = checkedAdd(stride, std::size_t{4});
        UF_CHECK(finalPixelOffset.has_value());
        writeBgraPixel(data, *finalPixelOffset, {10, 11, 12, 255});

        auto const result = bgra8ToAlpha8(data, 2, 2, stride);
        REQUIRE(result.has_value());
        auto const expected = std::vector<std::byte>{
            asByte(0),
            asByte(128),
            asByte(254),
            asByte(255),
        };
        CHECK(*result == expected);
    }

    TEST_CASE("an opaque mask reproduces the unmasked matcher exactly")
    {
        auto constexpr haystackWidth  = uint32{48};
        auto constexpr haystackHeight = uint32{36};
        auto constexpr paddedStride   = std::size_t{64};
        auto constexpr templateWidth  = uint32{9};
        auto constexpr templateHeight = uint32{7};
        auto constexpr plantedX       = uint32{11};
        auto constexpr plantedY       = uint32{5};

        auto const background = hashed(23);
        auto const haystackData = build(
            haystackWidth,
            haystackHeight,
            haystackWidth,
            asByte(0),
            background
        );
        auto const paddedData = build(
            haystackWidth,
            haystackHeight,
            paddedStride,
            asByte(0xFF),
            background
        );
        auto const plantedData = build(
            templateWidth,
            templateHeight,
            templateWidth,
            asByte(0),
            [background](uint32 x, uint32 y) noexcept -> uint8
            {
                return background(plantedX + x, plantedY + y);
            }
        );
        auto const foreignData = build(
            templateWidth,
            templateHeight,
            templateWidth,
            asByte(0),
            hashed(97)
        );
        auto const maskData = uniformMask(templateWidth, templateHeight, 255);

        auto const haystack = grayImage(
            haystackData,
            haystackWidth,
            haystackHeight,
            haystackWidth
        );
        auto const padded = grayImage(
            paddedData,
            haystackWidth,
            haystackHeight,
            paddedStride
        );
        auto const planted = grayImage(
            plantedData,
            templateWidth,
            templateHeight,
            templateWidth
        );
        auto const foreign = grayImage(
            foreignData,
            templateWidth,
            templateHeight,
            templateWidth
        );
        auto const mask = grayImage(
            maskData,
            templateWidth,
            templateHeight,
            templateWidth
        );

        auto const agree = [&mask](
            GrayImage const& searched,
            GrayImage const& templateImage,
            PixelRect roi,
            uint64 budget
        ) -> void
        {
            auto unmaskedPolls = uint32{0};
            auto maskedPolls   = uint32{0};
            auto const unmaskedPoll = SadSearchPoll{
                [&unmaskedPolls]() noexcept -> SadSearchControl
                {
                    ++unmaskedPolls;
                    return SadSearchControl::Continue;
                }
            };
            auto const maskedPoll = SadSearchPoll{
                [&maskedPolls]() noexcept -> SadSearchControl
                {
                    ++maskedPolls;
                    return SadSearchControl::Continue;
                }
            };

            auto const unmasked = matchTemplateSad(
                searched,
                templateImage,
                roi,
                budget,
                unmaskedPoll
            );
            auto const masked = matchTemplateSad(
                searched,
                templateImage,
                mask,
                roi,
                budget,
                maskedPoll
            );
            REQUIRE(unmasked.has_value());
            REQUIRE(masked.has_value());
            CHECK(masked->outcome == unmasked->outcome);
            CHECK(
                masked->completedPixelComparisons
                == unmasked->completedPixelComparisons
            );
            CHECK(maskedPolls == unmaskedPolls);
        };

        auto constexpr unlimited = std::numeric_limits<uint64>::max();
        auto const rois = std::array{
            pixelRect(0, 0, haystackWidth, haystackHeight),
            pixelRect(6, 4, 30, 24),
            pixelRect(0, 0, 4, 4),
        };
        for (auto const roi : rois)
        {
            agree(haystack, planted, roi, unlimited);
            agree(haystack, foreign, roi, unlimited);
            agree(padded, planted, roi, unlimited);
            agree(haystack, foreign, roi, 500);
        }
    }

    TEST_CASE("a masked label template survives a changed background")
    {
        struct LabelCase final
        {
            test::LabelFixture fixture{};

            uint64 maskedScore{};
            uint64 opaqueScore{};
        };

        auto const cases = std::array{
            LabelCase{test::k_sortieLabel, 1137, 227916},
            LabelCase{test::k_storyLabel, 661, 96551},
        };
        for (auto const& testCase : cases)
        {
            auto const& fixture     = testCase.fixture;
            auto const width        = fixture.templateWidth;
            auto const height       = fixture.templateHeight;
            auto const pixelCount   = uint64{width} * uint64{height};
            auto const templateData = decodeHex(fixture.templateHex);
            auto const haystackData = decodeHex(fixture.haystackHex);
            REQUIRE(templateData.size() == pixelCount);
            REQUIRE(
                haystackData.size()
                == uint64{fixture.haystackWidth} * uint64{fixture.haystackHeight}
            );

            auto const maskData   = nearWhiteMask(templateData);
            auto const opaqueData = uniformMask(width, height, 255);

            // The case only proves something while the glyph is a minority of
            // its rectangle: the rest is artwork that changed completely.
            CHECK(maskedPixelCount(maskData) * 2U < pixelCount);

            auto const templateImage = grayImage(templateData, width, height, width);
            auto const mask          = grayImage(maskData, width, height, width);
            auto const opaque        = grayImage(opaqueData, width, height, width);
            auto const haystack      = grayImage(
                haystackData,
                fixture.haystackWidth,
                fixture.haystackHeight,
                fixture.haystackWidth
            );
            auto const roi = pixelRect(
                0,
                0,
                fixture.haystackWidth,
                fixture.haystackHeight
            );
            auto const origin = test::k_labelTemplateOrigin;

            auto const masked = matchTemplateSad(haystack, templateImage, mask, roi);
            auto const unmaskable = matchTemplateSad(
                haystack,
                templateImage,
                opaque,
                roi
            );
            REQUIRE(masked.has_value());
            REQUIRE(unmaskable.has_value());
            REQUIRE(masked->has_value());
            REQUIRE(unmaskable->has_value());
            CHECK(
                **masked
                == SadMatch{origin, origin, testCase.maskedScore}
            );
            CHECK((*unmaskable)->score() == testCase.opaqueScore);

            // The largest score a 99% similarity threshold accepts over this
            // template rectangle, on the scale unmasked thresholds already use.
            auto const maximumSad = uint64{255} * pixelCount / 100U;
            CHECK((*masked)->score() <= maximumSad);
            CHECK((*unmaskable)->score() > maximumSad);

            // Control: over its own background the same template and mask are
            // exact, so the fixture crops really do hold the same label.
            auto const ownBackground = matchTemplateSad(
                templateImage,
                templateImage,
                mask,
                pixelRect(0, 0, width, height)
            );
            CHECK(matchScore(ownBackground) == 0);
        }
    }

    TEST_CASE("masked scores normalize by the weight actually summed")
    {
        auto constexpr extent = uint32{8};
        auto constexpr height = uint32{4};
        auto constexpr stable = uint32{4};
        auto constexpr pixels = uint64{extent} * uint64{height};

        // The left half differs by a constant 10 and the right half by 250, so
        // a mask that keeps only the left half must score exactly 10 per pixel
        // however much of that half it keeps.
        auto const templateData = build(
            extent,
            height,
            extent,
            asByte(0),
            [](uint32 x, uint32) noexcept -> uint32
            {
                return x < stable ? 100U : 0U;
            }
        );
        auto const haystackData = build(
            extent,
            height,
            extent,
            asByte(0),
            [](uint32 x, uint32) noexcept -> uint32
            {
                return x < stable ? 110U : 250U;
            }
        );
        auto const narrowData = build(
            extent,
            height,
            extent,
            asByte(0),
            [](uint32 x, uint32) noexcept -> uint32
            {
                return x == 0 ? 255U : 0U;
            }
        );
        auto const wideData = build(
            extent,
            height,
            extent,
            asByte(0),
            [](uint32 x, uint32) noexcept -> uint32
            {
                return x < stable ? 255U : 0U;
            }
        );
        auto const partialData = build(
            extent,
            height,
            extent,
            asByte(0),
            [](uint32 x, uint32) noexcept -> uint32
            {
                return x < stable ? 255U : 51U;
            }
        );

        auto const haystack = grayImage(haystackData, extent, height, extent);
        auto const templateImage = grayImage(templateData, extent, height, extent);
        auto const roi = pixelRect(0, 0, extent, height);
        auto const scoreWith = [&](std::vector<std::byte> const& maskData) -> uint64
        {
            auto const mask = grayImage(maskData, extent, height, extent);
            return matchScore(matchTemplateSad(haystack, templateImage, mask, roi));
        };

        // A quarter of the weight of the wide mask, and the same score.
        CHECK(maskedPixelCount(narrowData) * 4U == maskedPixelCount(wideData));
        CHECK(scoreWith(narrowData) == pixels * 10U);
        CHECK(scoreWith(wideData) == pixels * 10U);

        // Any uniform weight is the unmasked scale, because the constant
        // cancels out of the quotient.
        auto const unmasked = matchScore(
            matchTemplateSad(haystack, templateImage, roi)
        );
        CHECK(unmasked == pixels * 130U);
        CHECK(scoreWith(uniformMask(extent, height, 255)) == unmasked);
        CHECK(scoreWith(uniformMask(extent, height, 128)) == unmasked);
        CHECK(scoreWith(uniformMask(extent, height, 1)) == unmasked);

        // A partial weight lands between the two, in proportion to its weight.
        CHECK(scoreWith(partialData) == pixels * 50U);
    }

    TEST_CASE("a masked search keeps the budget, poll and cancellation contract")
    {
        auto const haystackData = std::vector<std::byte>{
            asByte(0),
            asByte(1),
            asByte(2),
        };
        auto const templateData = std::vector<std::byte>{asByte(255), asByte(7)};
        auto const maskData     = std::vector<std::byte>{asByte(0), asByte(255)};
        auto const haystack      = grayImage(haystackData, 3, 1, 3);
        auto const templateImage = grayImage(templateData, 2, 1, 2);
        auto const mask          = grayImage(maskData, 2, 1, 2);
        auto const roi           = pixelRect(0, 0, 3, 1);

        struct BudgetCase final
        {
            uint64 budget{};
            uint64 expectedComparisons{};
            uint32 expectedPolls{};
        };

        // An excluded pixel still consumes its comparison, so the budget keeps
        // measuring the rectangle the search walked rather than the weight.
        auto const budgets = std::array{
            BudgetCase{0, 0, 0},
            BudgetCase{1, 1, 1},
            BudgetCase{3, 3, 1},
        };
        for (auto const& testCase : budgets)
        {
            auto pollCount = uint32{0};
            auto const poll = SadSearchPoll{
                [&pollCount]() noexcept -> SadSearchControl
                {
                    ++pollCount;
                    return SadSearchControl::Continue;
                }
            };
            auto const result = matchTemplateSad(
                haystack,
                templateImage,
                mask,
                roi,
                testCase.budget,
                poll
            );
            REQUIRE(result.has_value());
            CHECK(
                std::get<SadSearchStopReason>(result->outcome)
                == SadSearchStopReason::ComparisonBudgetExhausted
            );
            CHECK(result->completedPixelComparisons == testCase.expectedComparisons);
            CHECK(pollCount == testCase.expectedPolls);
        }

        auto pollCount = uint32{0};
        auto const poll = SadSearchPoll{
            [&pollCount]() noexcept -> SadSearchControl
            {
                ++pollCount;
                return SadSearchControl::Continue;
            }
        };
        auto const completed = matchTemplateSad(
            haystack,
            templateImage,
            mask,
            roi,
            4,
            poll
        );
        REQUIRE(completed.has_value());
        CHECK(
            std::get<std::optional<SadMatch>>(completed->outcome)
            == std::optional{SadMatch{1, 0, 10}}
        );
        CHECK(completed->completedPixelComparisons == 4);

        struct InterruptionCase final
        {
            SadSearchControl    control{};
            SadSearchStopReason expected{};
        };

        auto const interruptions = std::array{
            InterruptionCase{
                SadSearchControl::Cancelled,
                SadSearchStopReason::Cancelled
            },
            InterruptionCase{
                SadSearchControl::TimedOut,
                SadSearchStopReason::TimedOut
            },
        };
        for (auto const& testCase : interruptions)
        {
            auto const stopping = SadSearchPoll{
                [control = testCase.control]() noexcept -> SadSearchControl
                {
                    return control;
                }
            };
            auto const result = matchTemplateSad(
                haystack,
                templateImage,
                mask,
                roi,
                4,
                stopping
            );
            REQUIRE(result.has_value());
            CHECK(
                std::get<SadSearchStopReason>(result->outcome)
                == testCase.expected
            );
            CHECK(result->completedPixelComparisons == 0);
        }

        auto constexpr wideWidth = uint32{4097};
        auto const wideData = std::vector<std::byte>(wideWidth, asByte(0));
        auto const singleData = std::vector<std::byte>{asByte(255)};
        auto const singleMaskData = std::vector<std::byte>{asByte(255)};
        auto const wide = grayImage(wideData, wideWidth, 1, wideWidth);
        auto const single = grayImage(singleData, 1, 1, 1);
        auto const singleMask = grayImage(singleMaskData, 1, 1, 1);
        auto intervalPolls = uint32{0};
        auto const cancelOnSecondPoll = SadSearchPoll{
            [&intervalPolls]() noexcept -> SadSearchControl
            {
                ++intervalPolls;
                if (intervalPolls == 2)
                {
                    return SadSearchControl::Cancelled;
                }
                return SadSearchControl::Continue;
            }
        };

        auto const interval = matchTemplateSad(
            wide,
            single,
            singleMask,
            pixelRect(0, 0, wideWidth, 1),
            uint64{wideWidth},
            cancelOnSecondPoll
        );
        REQUIRE(interval.has_value());
        CHECK(
            std::get<SadSearchStopReason>(interval->outcome)
            == SadSearchStopReason::Cancelled
        );
        CHECK(
            interval->completedPixelComparisons
            == k_sadSearchPollIntervalComparisons
        );
        CHECK(intervalPolls == 2);
    }

    TEST_CASE("an unusable template mask is rejected")
    {
        auto const haystackData  = std::vector<std::byte>{asByte(0), asByte(1)};
        auto const templateData  = std::vector<std::byte>{asByte(255), asByte(7)};
        auto const haystack      = grayImage(haystackData, 2, 1, 2);
        auto const templateImage = grayImage(templateData, 2, 1, 2);
        auto const roi           = pixelRect(0, 0, 2, 1);

        auto const mismatchedData = std::vector<std::byte>{asByte(255)};
        auto const mismatched = grayImage(mismatchedData, 1, 1, 1);
        auto const mismatchedResult = matchTemplateSad(
            haystack,
            templateImage,
            mismatched,
            roi
        );
        REQUIRE_FALSE(mismatchedResult.has_value());
        requireErrorKind(
            mismatchedResult.error(),
            AutomationErrorKind::InternalInvariant
        );

        auto const emptyData = std::vector<std::byte>{asByte(0), asByte(0)};
        auto const empty = grayImage(emptyData, 2, 1, 2);
        auto const emptyResult = matchTemplateSad(
            haystack,
            templateImage,
            empty,
            roi
        );
        REQUIRE_FALSE(emptyResult.has_value());
        requireErrorKind(
            emptyResult.error(),
            AutomationErrorKind::InternalInvariant
        );
    }

    // Helpers for the frame-analysis cases below; nothing above needs colour.
    namespace
    {
        [[nodiscard]]
        auto bgraImage(
            std::vector<std::byte> const& data UF_LIFETIME_BOUND,
            uint32 width,
            uint32 height
        ) -> BgraImage
        {
            auto const result = BgraImage::create(
                std::span<std::byte const>{data},
                width,
                height,
                std::size_t{width} * 4U
            );
            REQUIRE(result.has_value());
            return *result;
        }

        [[nodiscard]]
        auto bgraFromRgbHex(std::string_view hex) -> std::vector<std::byte>
        {
            auto const rgb = decodeHex(hex);
            REQUIRE(rgb.size() % 3 == 0);
            auto bgra = std::vector<std::byte>{};
            bgra.reserve(rgb.size() / 3U * 4U);
            for (auto index = std::size_t{0}; index < rgb.size(); index += 3)
            {
                bgra.emplace_back(checkedAt(rgb, index + 2));
                bgra.emplace_back(checkedAt(rgb, index + 1));
                bgra.emplace_back(checkedAt(rgb, index));
                bgra.emplace_back(asByte(255));
            }
            return bgra;
        }

        // A grey plane widened into BGRA8. The BT.601 weights sum to 256, so a
        // grey written to all three channels reads back exactly: measurements
        // over a widened frame are exact. Only its colour is gone, which is why
        // the colour cases below use the real RGB planes instead.
        [[nodiscard]]
        auto bgraFromGray(std::vector<std::byte> const& gray) -> std::vector<std::byte>
        {
            auto bgra = std::vector<std::byte>{};
            bgra.reserve(gray.size() * 4U);
            for (auto const value : gray)
            {
                bgra.emplace_back(value);
                bgra.emplace_back(value);
                bgra.emplace_back(value);
                bgra.emplace_back(asByte(255));
            }
            return bgra;
        }

        // Each label haystack is the same source rect as its template, grown by
        // k_labelTemplateOrigin on every side, so this cut puts the two crops
        // back onto one rectangle of the game screen.
        [[nodiscard]]
        auto cropToTemplateRect(
            std::vector<std::byte> const& plane,
            test::LabelFixture const& fixture
        ) -> std::vector<std::byte>
        {
            auto const origin = std::size_t{test::k_labelTemplateOrigin};
            auto cropped = std::vector<std::byte>{};
            cropped.reserve(
                std::size_t{fixture.templateWidth} * fixture.templateHeight
            );
            for (auto y = uint32{0}; y < fixture.templateHeight; ++y)
            {
                auto const row = (origin + y) * fixture.haystackWidth;
                for (auto x = uint32{0}; x < fixture.templateWidth; ++x)
                {
                    cropped.emplace_back(checkedAt(plane, row + origin + x));
                }
            }
            return cropped;
        }

        [[nodiscard]]
        auto stabilityOf(
            std::span<BgraImage const> frames,
            StabilitySpec const& spec
        ) -> StabilityReport
        {
            auto result = analyseStability(frames, spec);
            REQUIRE(result.has_value());
            return std::move(*result);
        }

        [[nodiscard]]
        auto probeOf(
            std::span<BgraImage const> frames,
            ColourProbeSpec const& spec
        ) -> ColourProbeReport
        {
            auto const result = probeColour(frames, spec);
            REQUIRE(result.has_value());
            return *result;
        }

        struct RegionShape final
        {
            uint32 x{};
            uint32 y{};
            uint32 width{};
            uint32 height{};
            uint64 stablePixels{};

            auto operator==(RegionShape const&) const -> bool = default;
        };

        [[nodiscard]]
        auto shapesOf(std::vector<StableRegion> const& regions) -> std::vector<RegionShape>
        {
            auto shapes = std::vector<RegionShape>{};
            shapes.reserve(regions.size());
            for (auto const& region : regions)
            {
                shapes.emplace_back(
                    RegionShape{
                        .x            = region.bounds.x(),
                        .y            = region.bounds.y(),
                        .width        = region.bounds.width(),
                        .height       = region.bounds.height(),
                        .stablePixels = region.stablePixels,
                    }
                );
            }
            return shapes;
        }

        constexpr auto k_bandedWidth  = uint32{40};
        constexpr auto k_bandedHeight = uint32{60};

        // Two stable bands over a background that differs in every pixel of
        // every frame: rows 5..20 and rows 31..45, a ten row gap apart, which is
        // the gap the 「故事」 entry has at y 166..175 on the real screen. The
        // third frame repeats the second's background and moves one band pixel,
        // the animation a two-frame comparison cannot see.
        struct BandedFrames final
        {
            std::vector<std::byte> first{};
            std::vector<std::byte> second{};
            std::vector<std::byte> third{};
        };

        [[nodiscard]]
        auto bandedFrames() -> BandedFrames
        {
            auto const toByte = [](uint32 value) -> std::byte
            {
                auto const narrowed = checkedCast<uint8>(value);
                REQUIRE(narrowed.has_value());
                return asByte(*narrowed);
            };

            auto frames = BandedFrames{};
            for (auto y = uint32{0}; y < k_bandedHeight; ++y)
            {
                for (auto x = uint32{0}; x < k_bandedWidth; ++x)
                {
                    auto const inBand = (
                        x >= 6U
                        && x < 34U
                        && (
                            (y >= 5U && y < 21U)
                            || (y >= 31U && y < 46U)
                        )
                    );
                    if (inBand)
                    {
                        auto const stroke = toByte((x + y) % 3U != 0 ? 250U : 240U);
                        frames.first.emplace_back(stroke);
                        frames.second.emplace_back(stroke);
                        frames.third.emplace_back(stroke);
                        continue;
                    }

                    auto const artwork = (x * 7U + y * 13U) % 200U;
                    frames.first.emplace_back(toByte(artwork));
                    frames.second.emplace_back(toByte((artwork + 17U) % 256U));
                    frames.third.emplace_back(toByte((artwork + 41U) % 256U));
                }
            }

            auto const moved = std::size_t{10} * k_bandedWidth + 10U;
            REQUIRE(checkedAt(frames.first, moved) == asByte(250));
            checkedAt(frames.third, moved) = asByte(99);
            return frames;
        }
    }

    TEST_CASE("stability over two real screenshots keeps the glyph and drops the artwork")
    {
        auto const& colour = test::k_sortieColourLabel;
        auto const blueData = bgraFromRgbHex(colour.overBlueArtworkRgbHex);
        auto const purpleData = bgraFromRgbHex(colour.overPurpleArtworkRgbHex);

        // The colour planes have to be the same two crops the grey fixture
        // holds, or every number below measures a different rectangle.
        auto const blueGray = bgra8ToGray8(
            std::span<std::byte const>{blueData},
            colour.width,
            colour.height,
            std::size_t{colour.width} * 4U
        );
        REQUIRE(blueGray.has_value());
        CHECK(*blueGray == decodeHex(test::k_sortieLabel.templateHex));

        auto const blue   = bgraImage(blueData, colour.width, colour.height);
        auto const purple = bgraImage(purpleData, colour.width, colour.height);
        auto const frames = std::array{blue, purple};
        auto const rect   = pixelRect(0, 0, colour.width, colour.height);

        auto const report = stabilityOf(
            frames,
            StabilitySpec{
                .rect          = rect,
                .grayTolerance = 0,
                .minimumGap    = 0,
            }
        );

        // Measured by hand over these two screenshots: the rectangle is about
        // 90% artwork, and what survives is the glyph.
        CHECK(report.rectPixels == 4000);
        CHECK(report.stablePixels == 461);
        CHECK(report.meanGraySpread == doctest::Approx(56.979).epsilon(1e-9));

        // Every pixel the UI drew in its own white is identical across the two
        // backgrounds. Without this the count above could be 461 coincidences.
        auto whitePixels = uint64{0};
        for (auto y = uint32{0}; y < colour.height; ++y)
        {
            for (auto x = uint32{0}; x < colour.width; ++x)
            {
                auto const pixel = blue.pixelAt(x, y);
                auto const white = Bgra8Pixel{
                    .blue  = 255,
                    .green = 255,
                    .red   = 255,
                    .alpha = 255,
                };
                if (pixel != white)
                {
                    continue;
                }
                ++whitePixels;
                CHECK(
                    checkedAt(report.stableMask, std::size_t{y} * colour.width + x)
                    == asByte(255)
                );
            }
        }
        CHECK(whitePixels == 311);

        // One region, hugging the glyph rather than the analysed rect. Add the
        // crop's own origin of (1380, 220) and its left edge is x = 1414, which
        // is where the 「出擊」 entry was measured to start.
        REQUIRE(report.regions.size() == 1);
        CHECK(
            shapesOf(report.regions).front()
            == RegionShape{
                .x            = 34,
                .y            = 9,
                .width        = 50,
                .height       = 24,
                .stablePixels = 461,
            }
        );

        // Control: a frame paired with itself is stable everywhere, so the
        // numbers above are the two backgrounds and not a comparison that fails.
        auto const identical = std::array{blue, blue};
        auto const selfReport = stabilityOf(
            identical,
            StabilitySpec{
                .rect          = rect,
                .grayTolerance = 0,
                .minimumGap    = 0,
            }
        );
        CHECK(selfReport.stablePixels == 4000);
        CHECK(selfReport.meanGraySpread == doctest::Approx(0.0));
    }

    TEST_CASE("a projection gap separates neighbouring labels that one box would merge")
    {
        auto const& colour = test::k_sortieColourLabel;
        auto const blueData = bgraFromRgbHex(colour.overBlueArtworkRgbHex);
        auto const purpleData = bgraFromRgbHex(colour.overPurpleArtworkRgbHex);
        auto const frames = std::array{
            bgraImage(blueData, colour.width, colour.height),
            bgraImage(purpleData, colour.width, colour.height),
        };
        auto const rect = pixelRect(0, 0, colour.width, colour.height);

        auto const split = [&](uint32 minimumGap) -> std::vector<RegionShape>
        {
            return shapesOf(
                stabilityOf(
                    frames,
                    StabilitySpec{
                        .rect          = rect,
                        .grayTolerance = 0,
                        .minimumGap    = minimumGap,
                    }
                ).regions
            );
        };

        // The two glyphs of 「出擊」 are four fully unstable columns apart, so
        // the cut finds them and the naive box does not.
        auto const separated = split(4);
        REQUIRE(separated.size() == 2);
        CHECK(
            checkedAt(separated, 0)
            == RegionShape{
                .x            = 34,
                .y            = 9,
                .width        = 21,
                .height       = 24,
                .stablePixels = 213,
            }
        );
        CHECK(
            checkedAt(separated, 1)
            == RegionShape{
                .x            = 59,
                .y            = 9,
                .width        = 25,
                .height       = 24,
                .stablePixels = 248,
            }
        );

        // Control: one column more than the gap and the same pixels come back
        // as the single merged box, so the split above is the gap being found
        // rather than the algorithm always cutting.
        auto const merged = split(5);
        REQUIRE(merged.size() == 1);
        CHECK(checkedAt(merged, 0).width == 50);
        CHECK(checkedAt(merged, 0).stablePixels == 461);

        // The projections are reported, so a caller can choose the gap from the
        // data instead of guessing it.
        auto const report = stabilityOf(
            frames,
            StabilitySpec{
                .rect          = rect,
                .grayTolerance = 0,
                .minimumGap    = 4,
            }
        );
        REQUIRE(report.columnProfile.size() == colour.width);
        REQUIRE(report.rowProfile.size() == colour.height);
        for (auto column = std::size_t{55}; column < 59; ++column)
        {
            CHECK(checkedAt(report.columnProfile, column) == 0);
        }
        CHECK(checkedAt(report.columnProfile, 54) > 0);
        CHECK(checkedAt(report.columnProfile, 59) > 0);

        // The label over sub-label case, on the axis the real crops are too
        // short to show: two bands ten rows apart, which is the gap 「故事」 has
        // above 「完成第5章」.
        auto const banded = bandedFrames();
        auto const first = bgraFromGray(banded.first);
        auto const second = bgraFromGray(banded.second);
        auto const bandFrames = std::array{
            bgraImage(first, k_bandedWidth, k_bandedHeight),
            bgraImage(second, k_bandedWidth, k_bandedHeight),
        };
        auto const bandRect = pixelRect(0, 0, k_bandedWidth, k_bandedHeight);
        auto const bandSplit = [&](uint32 minimumGap) -> std::vector<RegionShape>
        {
            return shapesOf(
                stabilityOf(
                    bandFrames,
                    StabilitySpec{
                        .rect          = bandRect,
                        .grayTolerance = 0,
                        .minimumGap    = minimumGap,
                    }
                ).regions
            );
        };

        auto const bands = bandSplit(10);
        REQUIRE(bands.size() == 2);
        CHECK(
            checkedAt(bands, 0)
            == RegionShape{
                .x            = 6,
                .y            = 5,
                .width        = 28,
                .height       = 16,
                .stablePixels = 448,
            }
        );
        CHECK(
            checkedAt(bands, 1)
            == RegionShape{
                .x            = 6,
                .y            = 31,
                .width        = 28,
                .height       = 15,
                .stablePixels = 420,
            }
        );

        // Controls on both sides: one row more than the gap merges the two
        // bands, and so does asking for no split at all.
        for (auto const gap : std::array{uint32{11}, uint32{0}})
        {
            auto const oneBox = bandSplit(gap);
            REQUIRE(oneBox.size() == 1);
            CHECK(
                checkedAt(oneBox, 0)
                == RegionShape{
                    .x            = 6,
                    .y            = 5,
                    .width        = 28,
                    .height       = 41,
                    .stablePixels = 868,
                }
            );
        }
    }

    TEST_CASE("a colour probe separates an anchor from its background")
    {
        auto const& colour = test::k_sortieColourLabel;
        auto const blueData = bgraFromRgbHex(colour.overBlueArtworkRgbHex);
        auto const purpleData = bgraFromRgbHex(colour.overPurpleArtworkRgbHex);
        auto const frames = std::array{
            bgraImage(blueData, colour.width, colour.height),
            bgraImage(purpleData, colour.width, colour.height),
        };
        auto const rect = pixelRect(0, 0, colour.width, colour.height);

        // Measured by hand over the same two screenshots at the same key and
        // tolerance: whole rect 56.98, masked 0.046.
        auto const sortie = probeOf(
            frames,
            ColourProbeSpec{
                .rect      = rect,
                .keyRed    = 255,
                .keyGreen  = 255,
                .keyBlue   = 255,
                .tolerance = 12,
            }
        );
        CHECK(sortie.rectPixels == 4000);
        CHECK(sortie.fullySelectedPixels == 328);
        CHECK(sortie.rampSelectedPixels == 32);
        CHECK(sortie.rectMeanGraySpread == doctest::Approx(56.979).epsilon(1e-9));
        CHECK(sortie.maskedMeanGraySpread == doctest::Approx(0.045696).epsilon(1e-5));

        // The pair is the answer: the key holds the rect's disagreement down by
        // three orders of magnitude, which is what makes it an anchor.
        CHECK(sortie.maskedMeanGraySpread * 1000.0 < sortie.rectMeanGraySpread);

        // The 「故事」 entry, whose fixture is grey only. Its whole-rect number
        // was measured by hand as 26.82.
        auto const& storyFixture = test::k_storyLabel;
        auto const storyTemplate = bgraFromGray(decodeHex(storyFixture.templateHex));
        auto const storyHaystack = bgraFromGray(
            cropToTemplateRect(decodeHex(storyFixture.haystackHex), storyFixture)
        );
        auto const storyFrames = std::array{
            bgraImage(storyTemplate, storyFixture.templateWidth, storyFixture.templateHeight),
            bgraImage(storyHaystack, storyFixture.templateWidth, storyFixture.templateHeight),
        };
        auto const story = probeOf(
            storyFrames,
            ColourProbeSpec{
                .rect = pixelRect(
                    0,
                    0,
                    storyFixture.templateWidth,
                    storyFixture.templateHeight
                ),
                .keyRed    = 255,
                .keyGreen  = 255,
                .keyBlue   = 255,
                .tolerance = 12,
            }
        );
        CHECK(story.rectPixels == 3600);
        CHECK(story.rectMeanGraySpread == doctest::Approx(26.819722).epsilon(1e-6));
        CHECK(story.maskedMeanGraySpread == doctest::Approx(0.019501).epsilon(1e-4));

        // Control: a key nothing in the rect is near selects nothing, and the
        // masked mean does not silently become the rect mean.
        auto const missing = probeOf(
            frames,
            ColourProbeSpec{
                .rect      = rect,
                .keyRed    = 0,
                .keyGreen  = 255,
                .keyBlue   = 0,
                .tolerance = 0,
            }
        );
        CHECK(missing.fullySelectedPixels == 0);
        CHECK(missing.rampSelectedPixels == 0);
        CHECK(missing.selectedWeight == 0);
        CHECK(missing.maskedMeanGraySpread == doctest::Approx(0.0));
        CHECK(missing.rectMeanGraySpread == doctest::Approx(56.979).epsilon(1e-9));
    }

    TEST_CASE("a key that names what to remove selects exactly the rest")
    {
        // The reading a multi-coloured mark needs. One colour cannot select an
        // emblem of a dozen hues, but its BACKDROP is one colour, so the same
        // rule selects the mark when it is read the other way round. Without
        // this such a mark can only be cut unmasked, and an unmasked template is
        // mostly backdrop, which is how a template comes to match every screen
        // (docs/pitfalls/colour-key-annotation.md).
        auto const& colour = test::k_sortieColourLabel;
        auto const blueData = bgraFromRgbHex(colour.overBlueArtworkRgbHex);
        auto const purpleData = bgraFromRgbHex(colour.overPurpleArtworkRgbHex);
        auto const frames = std::array{
            bgraImage(blueData, colour.width, colour.height),
            bgraImage(purpleData, colour.width, colour.height),
        };
        auto const rect = pixelRect(0, 0, colour.width, colour.height);

        auto const key = ColourProbeSpec{
            .rect      = rect,
            .keyRed    = 255,
            .keyGreen  = 255,
            .keyBlue   = 255,
            .tolerance = 12,
        };
        auto const kept = probeOf(frames, key);

        auto inverted       = key;
        inverted.keyRemoves = true;
        auto const dropped = probeOf(frames, inverted);

        // Exactly complementary, and the arithmetic is the assertion: what the
        // white key took at full weight is what the same key now refuses, and
        // every remaining pixel is taken.
        CHECK(dropped.rectPixels == kept.rectPixels);
        CHECK(
            dropped.fullySelectedPixels
            == kept.rectPixels - kept.fullySelectedPixels - kept.rampSelectedPixels
        );
        CHECK(dropped.fullySelectedPixels == 3640);

        // The rim is still the rim. A weight strictly between nothing and all
        // stays strictly between when complemented, so an antialiased edge is
        // readmitted at the weight it deserves on the side it is now on -- it
        // does not collapse into either bucket.
        CHECK(dropped.rampSelectedPixels == kept.rampSelectedPixels);
        CHECK(dropped.rampSelectedPixels == 32);

        // And the whole-rect measurement is untouched: the key decides what is
        // weighed, never what the pixels are.
        CHECK(
            dropped.rectMeanGraySpread
            == doctest::Approx(kept.rectMeanGraySpread).epsilon(1e-9)
        );

        // The far end of the same rule. A colour nothing in the rect is near
        // selects nothing when kept -- the control above -- and everything when
        // removed, with no ramp because no pixel is anywhere near the boundary.
        auto absent = ColourProbeSpec{
            .rect      = rect,
            .keyRed    = 0,
            .keyGreen  = 255,
            .keyBlue   = 0,
            .tolerance = 0,
        };
        absent.keyRemoves = true;
        auto const everything = probeOf(frames, absent);
        CHECK(everything.fullySelectedPixels == everything.rectPixels);
        CHECK(everything.rampSelectedPixels == 0);
    }

    TEST_CASE("the mirrored colour-key ramp holds full weight then falls to nothing")
    {
        // The values were measured against the authoring-side ColourKey that
        // retired with the annotation module; colourKeyAlpha is now the rule's
        // one implementation, so a change to it shows up as one of these moving.
        auto const white = std::array{
            std::pair{uint32{0}, uint8{255}},
            std::pair{uint32{12}, uint8{255}},
            std::pair{uint32{13}, uint8{234}},
            std::pair{uint32{18}, uint8{128}},
            std::pair{uint32{23}, uint8{21}},
            std::pair{uint32{24}, uint8{0}},
            std::pair{uint32{30}, uint8{0}},
        };
        for (auto const& [distance, expected] : white)
        {
            auto const red = checkedCast<uint8>(255U - distance);
            REQUIRE(red.has_value());
            CHECK(
                colourKeyAlpha(
                    Bgra8Pixel{
                        .blue  = 255,
                        .green = 255,
                        .red   = *red,
                        .alpha = 255,
                    },
                    255,
                    255,
                    255,
                    12
                )
                == expected
            );
        }

        // A tolerance of zero has no ramp and stays an exact-colour match.
        auto const exact = Bgra8Pixel{
            .blue  = 255,
            .green = 255,
            .red   = 255,
            .alpha = 255,
        };
        auto const offByOne = Bgra8Pixel{
            .blue  = 255,
            .green = 255,
            .red   = 254,
            .alpha = 255,
        };
        CHECK(colourKeyAlpha(exact, 255, 255, 255, 0) == 255);
        CHECK(colourKeyAlpha(offByOne, 255, 255, 255, 0) == 0);
    }

    TEST_CASE("a colour census names the UI white without sampling a pixel")
    {
        auto const& colour = test::k_sortieColourLabel;
        auto const blueData = bgraFromRgbHex(colour.overBlueArtworkRgbHex);
        auto const blue = bgraImage(blueData, colour.width, colour.height);

        auto const result = censusColours(
            blue,
            ColourCensusSpec{
                .rect           = pixelRect(0, 0, colour.width, colour.height),
                .maximumEntries = 4,
            }
        );
        REQUIRE(result.has_value());
        CHECK(result->rectPixels == 4000);
        REQUIRE(result->dominant.size() == 4);

        // Sampling single pixels by hand produced (249, 249, 249) and
        // (247, 247, 247); the census names the actual UI white and says how
        // much of the rectangle it owns.
        CHECK(
            result->dominant.front()
            == ColourCount{
                .blue  = 255,
                .green = 255,
                .red   = 255,

                .count = 311,
            }
        );
        for (auto index = std::size_t{1}; index < result->dominant.size(); ++index)
        {
            CHECK(checkedAt(result->dominant, index).count <= checkedAt(result->dominant, index - 1).count);
        }

        // The second colour is artwork, not another UI tone, which is what
        // makes the first entry worth reading as the key.
        CHECK(checkedAt(result->dominant, 1).count == 260);

        // Control: the rect is not one flat colour, so the entry above is a
        // majority rather than the only answer available.
        CHECK(result->distinctColours == 676);
        CHECK(result->distinctColours < result->rectPixels);
    }

    TEST_CASE("a third frame unstables a pixel the first two agreed on")
    {
        auto const banded = bandedFrames();
        auto const first = bgraFromGray(banded.first);
        auto const second = bgraFromGray(banded.second);
        auto const third = bgraFromGray(banded.third);
        auto const firstImage = bgraImage(first, k_bandedWidth, k_bandedHeight);
        auto const rect = pixelRect(0, 0, k_bandedWidth, k_bandedHeight);
        auto const spec = StabilitySpec{
            .rect          = rect,
            .grayTolerance = 0,
            .minimumGap    = 10,
        };
        auto const moved = std::size_t{10} * k_bandedWidth + 10U;

        auto const twoFrames = std::array{
            firstImage,
            bgraImage(second, k_bandedWidth, k_bandedHeight),
        };
        auto const pair = stabilityOf(twoFrames, spec);
        CHECK(pair.stablePixels == 868);
        CHECK(checkedAt(pair.stableMask, moved) == asByte(255));

        auto const threeFrames = std::array{
            firstImage,
            bgraImage(second, k_bandedWidth, k_bandedHeight),
            bgraImage(third, k_bandedWidth, k_bandedHeight),
        };
        auto const triple = stabilityOf(threeFrames, spec);
        CHECK(triple.stablePixels == 867);
        CHECK(checkedAt(triple.stableMask, moved) == asByte(0));

        // The animated pixel leaves its region rather than the region leaving
        // the report, which is the difference a caller acts on.
        auto const shapes = shapesOf(triple.regions);
        REQUIRE(shapes.size() == 2);
        CHECK(checkedAt(shapes, 0).stablePixels == 447);
        CHECK(checkedAt(shapes, 1).stablePixels == 420);

        // A single frame answers nothing and is refused rather than reported as
        // stable everywhere.
        auto const lone = std::array{firstImage};
        auto const loneResult = analyseStability(lone, spec);
        REQUIRE_FALSE(loneResult.has_value());
        requireErrorKind(
            loneResult.error(),
            AutomationErrorKind::InternalInvariant
        );
    }

    TEST_CASE("frame analysis stops on its budget and on a cancelling poll")
    {
        auto const banded = bandedFrames();
        auto const first = bgraFromGray(banded.first);
        auto const second = bgraFromGray(banded.second);
        auto const frames = std::array{
            bgraImage(first, k_bandedWidth, k_bandedHeight),
            bgraImage(second, k_bandedWidth, k_bandedHeight),
        };
        auto const rect = pixelRect(0, 0, k_bandedWidth, k_bandedHeight);
        auto const spec = StabilitySpec{
            .rect          = rect,
            .grayTolerance = 0,
            .minimumGap    = 10,
        };
        auto const keepGoing = SadSearchPoll{
            []() noexcept -> SadSearchControl
            {
                return SadSearchControl::Continue;
            }
        };

        auto const budgeted = analyseStability(frames, spec, 100, keepGoing);
        REQUIRE(budgeted.has_value());
        CHECK(
            std::get<SadSearchStopReason>(budgeted->outcome)
            == SadSearchStopReason::ComparisonBudgetExhausted
        );
        CHECK(budgeted->completedPixelVisits == 100);

        auto const cancelled = analyseStability(
            frames,
            spec,
            std::numeric_limits<uint64>::max(),
            SadSearchPoll{
                []() noexcept -> SadSearchControl
                {
                    return SadSearchControl::Cancelled;
                }
            }
        );
        REQUIRE(cancelled.has_value());
        CHECK(
            std::get<SadSearchStopReason>(cancelled->outcome)
            == SadSearchStopReason::Cancelled
        );
        CHECK(cancelled->completedPixelVisits == 0);

        // Control: the same call without a stop reaches the end and counts one
        // visit per pixel per frame, so the two stops above are the budget and
        // the poll rather than the scan failing on its own.
        auto const complete = analyseStability(
            frames,
            spec,
            std::numeric_limits<uint64>::max(),
            keepGoing
        );
        REQUIRE(complete.has_value());
        CHECK(std::holds_alternative<StabilityReport>(complete->outcome));
        CHECK(
            complete->completedPixelVisits
            == uint64{k_bandedWidth} * k_bandedHeight * 2U
        );

        auto const probeBudget = probeColour(
            frames,
            ColourProbeSpec{
                .rect      = rect,
                .keyRed    = 250,
                .keyGreen  = 250,
                .keyBlue   = 250,
                .tolerance = 12,
            },
            100,
            keepGoing
        );
        REQUIRE(probeBudget.has_value());
        CHECK(
            std::get<SadSearchStopReason>(probeBudget->outcome)
            == SadSearchStopReason::ComparisonBudgetExhausted
        );
    }
}
