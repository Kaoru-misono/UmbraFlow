#include <ocr/engine.hpp>
#include <ocr/onnx-engine.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <image/png.hpp>

#include <vision/bgra-image.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#ifndef UF_REAL_REGRESSION_ROOT
#error "UF_REAL_REGRESSION_ROOT must be defined to build the real-regression test"
#endif
#ifndef UF_OCR_MODEL_ROOT
#error "UF_OCR_MODEL_ROOT must be defined to build the ocr real-regression test"
#endif

// What the OCR adapter actually reads off a real frame of this project's target.
//
// Local-only, for the reason every real-screenshot test here is: the fixture is
// a game screenshot that is never committed, so CI has nothing to run this
// against. It exists because the alternative -- a synthetic image of text --
// would prove that the pipeline runs without proving it reads anything, and the
// whole engine choice was made on what it reads.
//
// The expectations are exact strings rather than "not empty". An OCR that
// returns plausible nonsense fails nothing under a looser assertion, and
// plausible nonsense is this component's characteristic failure.
namespace uf::ocr
{
    namespace
    {
        // One region of the fixture and the text a human reads there.
        struct ReadCase final
        {
            std::string_view name;
            PixelRect        rect;
            std::string_view expected;
        };

        [[nodiscard]] auto rectOf(uint32 x, uint32 y, uint32 w, uint32 h) -> PixelRect
        {
            auto rect = PixelRect::create(x, y, w, h);
            REQUIRE(rect.has_value());
            return *rect;
        }

        // The decoded fixture, kept as BGRA so a BgraImage can view it.
        //
        // The swizzle is load-bearing rather than housekeeping: image::loadPng
        // yields RGBA, the recognition model was trained on OpenCV's BGR, and a
        // model fed the wrong channel order returns confident nonsense instead of
        // failing. Captured frames arrive BGRA already, so this conversion is the
        // PNG path's alone.
        struct DecodedFixture final
        {
            std::vector<std::byte> pixels{};
            uint32                 width{};
            uint32                 height{};
        };

        [[nodiscard]]
        auto loadFixture(std::filesystem::path const& path) -> DecodedFixture
        {
            auto decoded = image::loadPng(path);
            REQUIRE_MESSAGE(decoded.has_value(), path.string());

            auto fixture = DecodedFixture{
                .pixels = std::move(decoded->pixels),
                .width  = decoded->width,
                .height = decoded->height,
            };
            for (auto index = std::size_t{0}; index + 3U < fixture.pixels.size(); index += 4U)
            {
                std::swap(fixture.pixels[index], fixture.pixels[index + 2U]);
            }
            return fixture;
        }
    }

    TEST_CASE("The ocr adapter reads this target's Traditional Chinese UI")
    {
        auto const modelRoot = std::filesystem::path{UF_OCR_MODEL_ROOT};
        auto engine          = createOnnxEngine(
            OnnxEngineConfig{
                .recognitionModel  = modelRoot / "ppocr-v6-small-rec" / "inference.onnx",
                .recognitionConfig = modelRoot / "ppocr-v6-small-rec" / "inference.yml",
            }
        );
        auto const reason = engine.has_value()
            ? std::string{}
            : std::string{engine.error().message()};
        REQUIRE_MESSAGE(engine.has_value(), reason);

        auto const fixture = loadFixture(
            std::filesystem::path{UF_REAL_REGRESSION_ROOT} / "ocr"
                / "safe-zone-1600x900.png"
        );
        auto const image = BgraImage::create(
            fixture.pixels,
            fixture.width,
            fixture.height,
            static_cast<std::size_t>(fixture.width) * 4U
        );
        REQUIRE(image.has_value());

        // Digits, a digit-and-separator pair, two-character labels and a full
        // sentence. The sentence is the one that matters most: it is the case a
        // template matcher can never cover, and 擇 in it is a character the
        // previous model generation's dictionary could not spell at all.
        auto const cases = std::vector<ReadCase>{
            {.name = "hp",       .rect = rectOf(262, 14, 138, 36),   .expected = "621/922"},
            {.name = "currency", .rect = rectOf(1240, 22, 80, 40),   .expected = "124"},
            {.name = "rest",     .rect = rectOf(488, 640, 82, 42),   .expected = "休息"},
            {.name = "free",     .rect = rectOf(503, 682, 55, 30),   .expected = "免費"},
            {.name = "skip",     .rect = rectOf(1465, 798, 95, 52),  .expected = "跳過"},
            {
                .name     = "hint",
                .rect     = rectOf(1080, 720, 460, 40),
                .expected = "請選擇1種想要在安全區域使用的功能",
            },
        };

        for (auto const& testCase : cases)
        {
            CAPTURE(testCase.name);
            auto readout = (*engine)->read(
                *image,
                ReadSpec{
                    .rect   = testCase.rect,
                    .layout = TextLayout::SingleLine,
                }
            );
            REQUIRE(readout.has_value());
            REQUIRE(readout->lines.size() == 1U);

            auto const& line = readout->lines.front();
            CHECK(line.text == testCase.expected);
            CHECK(line.bounds == testCase.rect);

            // A confidence floor, not a score to tune: every one of these read
            // at 0.95 and above when the engine was chosen, so a drop past this
            // means the pipeline changed rather than the frame did.
            CHECK(line.confidenceBp >= 9000U);
        }
    }

    TEST_CASE("A block read reports that detection is not wired yet")
    {
        auto const modelRoot = std::filesystem::path{UF_OCR_MODEL_ROOT};
        auto engine          = createOnnxEngine(
            OnnxEngineConfig{
                .recognitionModel  = modelRoot / "ppocr-v6-small-rec" / "inference.onnx",
                .recognitionConfig = modelRoot / "ppocr-v6-small-rec" / "inference.yml",
            }
        );
        REQUIRE(engine.has_value());

        auto const pixels = std::vector<std::byte>(4U * 4U * 4U, std::byte{0});
        auto const image  = BgraImage::create(pixels, 4, 4, 16);
        REQUIRE(image.has_value());

        // Stated as a test so the day detection lands, this one goes red and
        // names the file that has to change with it.
        auto const readout = (*engine)->read(*image, ReadSpec{.layout = TextLayout::Block});
        REQUIRE_FALSE(readout.has_value());
        CHECK(
            automationErrorKind(readout.error())
            == AutomationErrorKind::UnsupportedCapability
        );
    }
}
