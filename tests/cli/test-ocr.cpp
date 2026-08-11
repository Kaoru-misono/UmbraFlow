#include <cli/ocr.hpp>

#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <ocr/text.hpp>

#include <doctest/doctest.h>

#include <string>
#include <vector>

// What `umbra-flow ocr` prints, asserted as exact bytes.
//
// Exact rather than "contains the text": this document exists to be hashed and
// diffed by a caller that did not measure the pixels itself, so member order,
// escaping and number form are the contract and not the rendering. Every
// looser assertion passes against a document whose members moved.
namespace uf::cli
{
    namespace
    {
        [[nodiscard]]
        auto rectOf(uint32 x, uint32 y, uint32 width, uint32 height) -> PixelRect
        {
            auto rect = PixelRect::create(x, y, width, height);
            REQUIRE(rect.has_value());
            return *rect;
        }

        [[nodiscard]]
        auto lineOf(
            std::string text,
            PixelRect bounds,
            uint32 confidenceBp
        ) -> ocr::TextLine
        {
            return ocr::TextLine{
                .text         = std::move(text),
                .bounds       = bounds,
                .confidenceBp = confidenceBp,
            };
        }
    }

    // The measured page as a whole: the extent, then the lines in the order the
    // read produced them. The two rectangles below are the ones the real
    // regression fixture reads at, so the coordinates are image pixels a caller
    // could crop with rather than invented ones.
    TEST_CASE("formatImageText prints one canonical object per measurement")
    {
        auto const text = ImageText{
            .width  = 1600,
            .height = 900,
            .lines  = std::vector<ocr::TextLine>{
                lineOf("\xE4\xBC\x91\xE6\x81\xAF", rectOf(488, 640, 82, 42), 9926),
                lineOf("\xE5\x85\x8D\xE8\xB2\xBB", rectOf(503, 682, 55, 30), 9814),
            },
        };

        // Members sorted by name at every level -- image before lines, and
        // confidenceBp, rect, text inside a line -- which is the order RFC 8785
        // computes and not the order the emitter writes them in. Non-ASCII text
        // stays UTF-8 rather than becoming \u escapes, which is the same rule.
        CHECK(
            formatImageText(text)
            == "{\"image\":{\"height\":900,\"width\":1600},\"lines\":["
               "{\"confidenceBp\":9926,\"rect\":{\"height\":42,\"width\":82,"
               "\"x\":488,\"y\":640},\"text\":\"\xE4\xBC\x91\xE6\x81\xAF\"},"
               "{\"confidenceBp\":9814,\"rect\":{\"height\":30,\"width\":55,"
               "\"x\":503,\"y\":682},\"text\":\"\xE5\x85\x8D\xE8\xB2\xBB\"}]}"
        );
    }

    // Finding no text is an ordinary answer about an image, so the array is
    // present and empty rather than absent, and the extent is still stated. A
    // caller must be able to tell "nothing there" from "the read never ran",
    // and only the second is a failure.
    TEST_CASE("formatImageText answers an image holding no text with an empty array")
    {
        CHECK(
            formatImageText(ImageText{.width = 4, .height = 4})
            == "{\"image\":{\"height\":4,\"width\":4},\"lines\":[]}"
        );
    }

    // A recogniser reads whatever is on the glyphs, including the two
    // characters JSON reserves. Hand-assembled output would emit them raw and
    // produce a document that no longer parses -- silently, because nothing in
    // this verb reads its own output back.
    TEST_CASE("formatImageText escapes text a recogniser could legitimately return")
    {
        auto const text = ImageText{
            .width  = 64,
            .height = 32,
            .lines  = std::vector<ocr::TextLine>{
                lineOf("say \"hi\"\\now\t", rectOf(1, 2, 3, 4), 5000),
            },
        };

        CHECK(
            formatImageText(text)
            == "{\"image\":{\"height\":32,\"width\":64},\"lines\":["
               "{\"confidenceBp\":5000,\"rect\":{\"height\":4,\"width\":3,"
               "\"x\":1,\"y\":2},\"text\":\"say \\\"hi\\\"\\\\now\\t\"}]}"
        );
    }
}
