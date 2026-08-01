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

        // The recognition-only engine still exists and still refuses, because a
        // product that never reads a region must not pay ten megabytes for
        // weights it will not load. The refusal is by name rather than an empty
        // answer: an engine that cannot find lines has not established that the
        // region held none.
        auto const readout = (*engine)->read(*image, ReadSpec{.layout = TextLayout::Block});
        REQUIRE_FALSE(readout.has_value());
        CHECK(
            automationErrorKind(readout.error())
            == AutomationErrorKind::UnsupportedCapability
        );
    }

    // What the detector actually finds on a real frame of this project's target,
    // and where it says each line is.
    //
    // The region below is a panel holding several labels the case above already
    // reads one at a time, so the two halves are measured against the same
    // pixels: the single-line cases prove the recogniser, and this proves that
    // nobody had to tell it where to look. The expectations are exact strings
    // and rectangles that must CONTAIN the hand-measured ones, because both
    // failure modes of a detector are silent -- boxes in the wrong place read
    // plausible nonsense, and boxes in crop coordinates land a click one origin
    // away from the text.
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
        // Exactly the two labels a human reads in that panel. A count is worth
        // asserting here where it usually is not: the two ways a detector goes
        // wrong are splitting one label into pieces and merging two into one,
        // and both leave the strings below still findable in a longer list.
        REQUIRE(readout->lines.size() == 2U);

        // The two labels the single-line cases read at (488, 640, 82, 42) and
        // (503, 682, 55, 30). Naming them by their text and then checking the
        // rectangle is the order that matters: a detector that found the right
        // number of boxes in the wrong places would pass a count assertion.
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

            // TARGET pixels, not crop-relative. A box reported inside the crop
            // would sit near the origin and pass every "is it in range" test a
            // careless assertion could make, so this checks the interval the
            // hand-measured label actually occupies.
            CHECK(p_line->bounds.x() >= region.x());
            CHECK(p_line->bounds.y() >= region.y());
            CHECK(p_line->bounds.right() <= region.right());
            CHECK(p_line->bounds.bottom() <= region.bottom());

            // NEITHER RECTANGLE CONTAINS THE OTHER, and asserting that either
            // did would be wrong rather than strict. The detector's box hugs the
            // glyphs and then grows by the unclip distance; the hand-measured
            // rect was drawn with whatever padding made a single-line read work.
            // Measured, the two overlap and are offset by a few pixels each way.
            // What is true of two rectangles around one label and false of two
            // around different ones is that each holds the other's centre, so
            // that is what is checked -- it pins the box to this label without
            // pinning the unclip ratio through a proxy.
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


    // The case the block read exists for, on the fixture that already proves
    // the recogniser: one long sentence whose first and last glyphs sit at the
    // edges of whatever box the detector draws.
    //
    // It reads MORE than the single-line case above does. That case hands the
    // recogniser (1080, 720, 460, 40), a rectangle a human drew, and gets
    // "請選擇1種想要在安全區域使用的功能"; the detector finds the sentence's own
    // extent and the recogniser then reads the closing "。" the drawn rectangle
    // cut off. Which is the whole argument for the verb in one line: a
    // rectangle somebody drew is a guess about where text is, and a frame knows.
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

        // ONE line and not several. A detector whose boxes are too tight splits
        // a sentence at its widest gaps, and every piece would still be readable
        // text -- so the count is what catches it and the strings would not.
        REQUIRE(readout->lines.size() == 1U);

        auto const& line = readout->lines.front();
        CHECK(line.text == "請選擇1種想要在安全區域使用的功能。");
        CHECK(line.confidenceBp >= 9000U);
    }
}
