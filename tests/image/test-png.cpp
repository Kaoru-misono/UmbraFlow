#include "test-helpers.hpp"

#include <core/types/integer.hpp>
#include <core/utility/scope-exit.hpp>
#include <image/png.hpp>

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <string_view>
#include <system_error>
#include <vector>

namespace uf::image
{
    namespace
    {
        [[nodiscard]]
        constexpr auto asByte(unsigned int value) noexcept -> std::byte
        {
            return static_cast<std::byte>(value);
        }
    }

    TEST_CASE("image PNG encoder round-trips exact RGBA pixels through the decoder")
    {
        auto const now = std::chrono::steady_clock::now();
        auto const token = now.time_since_epoch().count();
        auto const filename = std::format("umbraflow-m0-demo-png-{}.png", token);
        auto const path = std::filesystem::temp_directory_path() / filename;
        auto const cleanupPath = std::make_shared<std::filesystem::path const>(path);
        auto const cleanup = scopeExit(
            [cleanupPath]() noexcept
            {
                auto error = std::error_code{};
                static_cast<void>(std::filesystem::remove(*cleanupPath, error));
            }
        );
        auto const pixels = std::vector{
            asByte(0xFF), asByte(0x00), asByte(0x00), asByte(0xFF),
            asByte(0x00), asByte(0xFF), asByte(0x00), asByte(0x80),
            asByte(0x00), asByte(0x00), asByte(0xFF), asByte(0x40),
            asByte(0x12), asByte(0x34), asByte(0x56), asByte(0x78),
        };

        auto const written = writeRgbaPng(
            path,
            2,
            2,
            pixels
        );
        REQUIRE(written.has_value());

        auto const decoded = loadPng(path);
        REQUIRE(decoded.has_value());
        CHECK(decoded->m_width == 2U);
        CHECK(decoded->m_height == 2U);
        CHECK(decoded->m_pixels == pixels);
    }

    TEST_CASE("image PNG encoder retains the operating-system cause for output failures")
    {
        auto const pixels = std::vector{
            asByte(0x12),
            asByte(0x34),
            asByte(0x56),
            asByte(0x78),
        };
        auto const result = writeRgbaPng(
            std::filesystem::temp_directory_path(),
            1,
            1,
            pixels
        );

        REQUIRE_FALSE(result.has_value());
        test_image::requireErrorKind(
            result.error(),
            AutomationErrorKind::IoFailure
        );
        auto const nativeCode = result.error().nativeCode();
        CHECK(nativeCode.value() != 0);
        CHECK_FALSE(nativeCode.message().empty());
        // The point of carrying a category is that this is distinguishable from
        // a Win32 status with the same numeric value.
        CHECK(nativeCode.category() != std::system_category());
        CHECK(result.error().message().find("failed to open PNG") != std::string_view::npos);
    }

    TEST_CASE("image PNG encoder rejects dimensions outside the decoder quotas")
    {
        struct OversizedImage final
        {
            uint32 m_width{};
            uint32 m_height{};
        };
        for (auto const image : std::array{
            OversizedImage{4'194'304U, 1U},
            OversizedImage{16'384U, 32'768U},
            OversizedImage{8'193U, 1U},
        })
        {
            auto const result = writeRgbaPng(
                "oversized.png",
                image.m_width,
                image.m_height,
                {}
            );

            REQUIRE_FALSE(result.has_value());
            test_image::requireErrorKind(
                result.error(),
                AutomationErrorKind::InvalidResource
            );
            CHECK(result.error().message().contains("exceed"));
        }
    }

    TEST_CASE("image PNG decoder fails closed for malformed and oversized resources")
    {
        auto const empty = std::vector<std::byte>{};
        auto const malformed = decodePng(empty, "empty.png");
        REQUIRE_FALSE(malformed.has_value());
        test_image::requireErrorKind(
            malformed.error(),
            AutomationErrorKind::InvalidResource
        );

        auto const oversizedHeader = std::array{
            asByte(0x89), asByte(0x50), asByte(0x4E), asByte(0x47),
            asByte(0x0D), asByte(0x0A), asByte(0x1A), asByte(0x0A),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x0D),
            asByte(0x49), asByte(0x48), asByte(0x44), asByte(0x52),
            asByte(0x00), asByte(0x00), asByte(0x20), asByte(0x01),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x01),
            asByte(0x08), asByte(0x06), asByte(0x00), asByte(0x00),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x00),
            asByte(0x00),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x00),
            asByte(0x49), asByte(0x44), asByte(0x41), asByte(0x54),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x00),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x00),
            asByte(0x49), asByte(0x45), asByte(0x4E), asByte(0x44),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x00),
        };
        auto const oversized = decodePng(
            oversizedHeader,
            "dimension-limit.png"
        );
        REQUIRE_FALSE(oversized.has_value());
        test_image::requireErrorKind(
            oversized.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK(
            oversized.error().message().find("8193x1 exceeds 8192 pixels per axis")
            != std::string_view::npos
        );
    }

    TEST_CASE("image PNG decoder rejects non-PNG and truncated chunk lengths")
    {
        auto const jpeg = std::array{
            asByte(0xFF),
            asByte(0xD8),
            asByte(0xFF),
            asByte(0xE0),
        };
        auto const nonPng = decodePng(jpeg, "template.jpg");
        REQUIRE_FALSE(nonPng.has_value());
        test_image::requireErrorKind(
            nonPng.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK(nonPng.error().message().find("not a PNG") != std::string_view::npos);

        auto const truncatedChunk = std::array{
            asByte(0x89), asByte(0x50), asByte(0x4E), asByte(0x47),
            asByte(0x0D), asByte(0x0A), asByte(0x1A), asByte(0x0A),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x0D),
            asByte(0x49), asByte(0x48), asByte(0x44), asByte(0x52),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x01),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x01),
            asByte(0x08), asByte(0x06), asByte(0x00), asByte(0x00),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x00),
            asByte(0x00),
            asByte(0x7F), asByte(0xFF), asByte(0xFF), asByte(0xFF),
            asByte(0x74), asByte(0x45), asByte(0x58), asByte(0x74),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x00),
        };
        auto const truncated = decodePng(
            truncatedChunk,
            "truncated-chunk.png"
        );
        REQUIRE_FALSE(truncated.has_value());
        test_image::requireErrorKind(
            truncated.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK(
            truncated.error().message().find("declared chunk length exceeds the input")
            != std::string_view::npos
        );
    }

    TEST_CASE("image 16-bit PNG downconversion uses round-to-nearest")
    {
        auto const encoded = std::array{
            asByte(0x89), asByte(0x50), asByte(0x4E), asByte(0x47),
            asByte(0x0D), asByte(0x0A), asByte(0x1A), asByte(0x0A),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x0D),
            asByte(0x49), asByte(0x48), asByte(0x44), asByte(0x52),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x01),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x01),
            asByte(0x10), asByte(0x06), asByte(0x00), asByte(0x00),
            asByte(0x00), asByte(0x4F), asByte(0x85), asByte(0x18),
            asByte(0xCA), asByte(0x00), asByte(0x00), asByte(0x00),
            asByte(0x11), asByte(0x49), asByte(0x44), asByte(0x41),
            asByte(0x54), asByte(0x78), asByte(0x9C), asByte(0x63),
            asByte(0x60), asByte(0x68), asByte(0x6C), asByte(0x68),
            asByte(0xF8), asByte(0xFF), asByte(0xBF), asByte(0x81),
            asByte(0x01), asByte(0x00), asByte(0x11), asByte(0x09),
            asByte(0x04), asByte(0x00), asByte(0x81), asByte(0xEE),
            asByte(0x58), asByte(0x57), asByte(0x00), asByte(0x00),
            asByte(0x00), asByte(0x00), asByte(0x49), asByte(0x45),
            asByte(0x4E), asByte(0x44), asByte(0xAE), asByte(0x42),
            asByte(0x60), asByte(0x82),
        };
        auto const decoded = decodePng(encoded, "rgba16.png");
        REQUIRE(decoded.has_value());
        CHECK(decoded->m_width == 1U);
        CHECK(decoded->m_height == 1U);
        auto const expected = std::vector{
            asByte(1),
            asByte(128),
            asByte(255),
            asByte(128),
        };
        CHECK(decoded->m_pixels == expected);
    }
}
