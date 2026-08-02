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
// Local-only: the fixture is a game screenshot that is never committed, so CI
// has nothing to run this against. The expectations are exact strings rather
// than "not empty" because plausible nonsense is this component's characteristic
// failure and passes any looser assertion.
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

        // The decoded fixture, kept as BGRA so a BgraImage can view it. The swizzle
        // is load-bearing: image::loadPng yields RGBA, the recognition model was
        // trained on OpenCV's BGR, and the wrong channel order returns confident
        // nonsense instead of failing. Captured frames arrive BGRA; this is PNG-only.
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

        // Digits, a digit-and-separator pair, two-character labels, and a
        // sentence a template matcher can never cover -- 擇 in it is a character
        // the previous model generation's dictionary could not spell at all.
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

            // A floor, not a score to tune: all of these read at 0.95 and above
            // when the engine was chosen, so a drop past it means the pipeline
            // changed, not the frame.
            CHECK(line.confidenceBp >= 9000U);
        }
    }

    TEST_CASE("An engine with no detection model refuses a block read by name")
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

        // The recognition-only engine exists because a product that never reads a
        // region must not pay ten megabytes for weights it will not load. It
        // refuses by name rather than empty: finding no lines is not establishing
        // that the region held none.
        auto const readout = (*engine)->read(*image, ReadSpec{.layout = TextLayout::Block});
        REQUIRE_FALSE(readout.has_value());
        CHECK(
            automationErrorKind(readout.error())
            == AutomationErrorKind::UnsupportedCapability
        );
    }

    // What the detector finds on a real frame of this project's target, and where
    // it says each line is. The region below is a panel of labels the case above
    // reads one at a time, so both halves are measured against the same pixels:
    // those prove the recogniser, this proves nobody had to tell it where to look.
    // Both failure modes of a detector are silent -- boxes in the wrong place read
    // plausible nonsense, and boxes in crop coordinates land a click one origin away.
    TEST_CASE("A block read locates this target's lines and reads each one")
    {
        auto const modelRoot = std::filesystem::path{UF_OCR_MODEL_ROOT};
        auto engine          = createOnnxEngine(
            OnnxEngineConfig{
                .recognitionModel  = modelRoot / "ppocr-v6-small-rec" / "inference.onnx",
                .recognitionConfig = modelRoot / "ppocr-v6-small-rec" / "inference.yml",
                .detectionModel    = modelRoot / "ppocr-v6-small-det" / "inference.onnx",
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

        auto const region  = rectOf(440, 600, 300, 140);
        auto const readout = (*engine)->read(
            *image,
            ReadSpec{
                .rect   = region,
                .layout = TextLayout::Block,
            }
        );
        auto const readReason = readout.has_value()
            ? std::string{}
            : std::string{readout.error().message()};
        REQUIRE_MESSAGE(readout.has_value(), readReason);
        // A count is worth asserting here where it usually is not: a detector goes
        // wrong by splitting one label into pieces or merging two into one, and
        // both leave the strings below still findable in a longer list.
        REQUIRE(readout->lines.size() == 2U);

        // Naming a line by its text and then checking its rectangle is the order
        // that matters: the right number of boxes in the wrong places passes the
        // count above.
        auto const findLine = [&readout](std::string_view text) -> TextLine const*
        {
            for (auto const& line : readout->lines)
            {
                if (line.text == text)
                {
                    return &line;
                }
            }
            return nullptr;
        };

        for (auto const& expected : std::vector<ReadCase>{
                 {.name = "rest", .rect = rectOf(488, 640, 82, 42), .expected = "休息"},
                 {.name = "free", .rect = rectOf(503, 682, 55, 30), .expected = "免費"},
             })
        {
            CAPTURE(expected.name);
            auto const* p_line = findLine(expected.expected);
            REQUIRE(p_line != nullptr);

            // TARGET pixels, not crop-relative: a box reported inside the crop
            // would sit near the origin and pass any careless range assertion.
            CHECK(p_line->bounds.x() >= region.x());
            CHECK(p_line->bounds.y() >= region.y());
            CHECK(p_line->bounds.right() <= region.right());
            CHECK(p_line->bounds.bottom() <= region.bottom());

            // Neither rectangle contains the other: the detector's box hugs the
            // glyphs then grows by the unclip distance, the hand-measured one
            // carries whatever padding made a single-line read work, and measured
            // they overlap offset by a few pixels each way. Each holding the other's
            // centre is true of two rectangles around one label and false of two
            // around different ones, and pins no unclip ratio by proxy.
            auto const holdsCentre = [](PixelRect const& outer, PixelRect const& inner)
            {
                auto const centreX = inner.x() + inner.width() / 2U;
                auto const centreY = inner.y() + inner.height() / 2U;
                return centreX >= outer.x() && centreX <= outer.right()
                    && centreY >= outer.y() && centreY <= outer.bottom();
            };
            CHECK(holdsCentre(p_line->bounds, expected.rect));
            CHECK(holdsCentre(expected.rect, p_line->bounds));

            CHECK(p_line->confidenceBp >= 9000U);
        }

        // Top to bottom, then left to right, which is Readout's contract and the
        // reason a caller may say "the first line that matches".
        for (auto index = std::size_t{1}; index < readout->lines.size(); ++index)
        {
            auto const& previous = readout->lines[index - 1U];
            auto const& current  = readout->lines[index];
            CHECK(
                (previous.bounds.y() < current.bounds.y()
                 || (previous.bounds.y() == current.bounds.y()
                     && previous.bounds.x() <= current.bounds.x()))
            );
        }
    }

    TEST_CASE("A block read past its line ceiling refuses instead of truncating")
    {
        auto const modelRoot = std::filesystem::path{UF_OCR_MODEL_ROOT};
        auto engine          = createOnnxEngine(
            OnnxEngineConfig{
                .recognitionModel  = modelRoot / "ppocr-v6-small-rec" / "inference.onnx",
                .recognitionConfig = modelRoot / "ppocr-v6-small-rec" / "inference.yml",
                .detectionModel    = modelRoot / "ppocr-v6-small-det" / "inference.onnx",
            }
        );
        REQUIRE(engine.has_value());

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

        // A ceiling of zero over a region that demonstrably holds lines. Remove
        // the maximumLines check in readBlock and this returns a readout.
        auto const readout = (*engine)->read(
            *image,
            ReadSpec{
                .rect         = rectOf(440, 600, 300, 140),
                .layout       = TextLayout::Block,
                .maximumLines = 0,
            }
        );
        REQUIRE_FALSE(readout.has_value());
        CHECK(
            automationErrorKind(readout.error())
            == AutomationErrorKind::RecognitionIncomplete
        );
    }


    // The block read reads MORE than the single-line case above: that one hands
    // the recogniser (1080, 720, 460, 40), a rectangle a human drew, and gets
    // "請選擇1種想要在安全區域使用的功能"; the detector finds the sentence's own
    // extent and the recogniser then reads the closing "。" the drawn rectangle
    // cut off.
    TEST_CASE("A block read finds a sentence a drawn rectangle would have clipped")
    {
        auto const modelRoot = std::filesystem::path{UF_OCR_MODEL_ROOT};
        auto engine          = createOnnxEngine(
            OnnxEngineConfig{
                .recognitionModel  = modelRoot / "ppocr-v6-small-rec" / "inference.onnx",
                .recognitionConfig = modelRoot / "ppocr-v6-small-rec" / "inference.yml",
                .detectionModel    = modelRoot / "ppocr-v6-small-det" / "inference.onnx",
            }
        );
        REQUIRE(engine.has_value());

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

        auto const readout = (*engine)->read(
            *image,
            ReadSpec{
                .rect   = rectOf(1060, 700, 500, 80),
                .layout = TextLayout::Block,
            }
        );
        REQUIRE(readout.has_value());

        // ONE line and not several: a detector whose boxes are too tight splits a
        // sentence at its widest gaps, and every piece still reads as text.
        REQUIRE(readout->lines.size() == 1U);

        auto const& line = readout->lines.front();
        CHECK(line.text == "請選擇1種想要在安全區域使用的功能。");
        CHECK(line.confidenceBp >= 9000U);
    }
}
