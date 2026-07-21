#include "test-helpers.hpp"

#include <ffi/png-decoder.hpp>
#include <ffi/png-encoder.hpp>

#include <core/types/integer.hpp>
#include <core/utility/scope-exit.hpp>

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

namespace
{
    [[nodiscard]]
    constexpr auto asByte(unsigned int value) noexcept -> std::byte
    {
        return static_cast<std::byte>(value);
    }
}

TEST_CASE("m0 PNG encoder round-trips exact RGBA pixels through the decoder")
{
    auto const now = std::chrono::steady_clock::now();
    auto const token = now.time_since_epoch().count();
    auto const filename = std::format("umbraflow-m0-demo-png-{}.png", token);
    auto const path = std::filesystem::temp_directory_path() / filename;
    auto const cleanupPath = std::make_shared<std::filesystem::path const>(path);
    auto const cleanup = uf::scopeExit(
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

    auto const written = uf::m0_demo::ffi::writeRgbaPng(
        path,
        2,
        2,
        pixels
    );
    REQUIRE(written.has_value());

    auto const decoded = uf::m0_demo::ffi::loadPng(path);
    REQUIRE(decoded.has_value());
    CHECK(decoded->m_width == 2U);
    CHECK(decoded->m_height == 2U);
    CHECK(decoded->m_pixels == pixels);
}

TEST_CASE("m0 PNG encoder retains the operating-system cause for output failures")
{
    auto const pixels = std::vector{
        asByte(0x12),
        asByte(0x34),
        asByte(0x56),
        asByte(0x78),
    };
    auto const result = uf::m0_demo::ffi::writeRgbaPng(
        std::filesystem::temp_directory_path(),
        1,
        1,
        pixels
    );

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code() == uf::ErrorCode::Io);
    test_m0_demo::requireErrorKind(
        result.error(),
        uf::AutomationErrorKind::InvalidResource
    );
    CHECK(result.error().nativeCode() != 0);
    CHECK(result.error().message().find("failed to open PNG") != std::string_view::npos);
}

TEST_CASE("m0 PNG encoder rejects dimensions outside the decoder quotas")
{
    struct OversizedImage final
    {
        uf::uint32 m_width;
        uf::uint32 m_height;
    };
    for (auto const image : std::array{
        OversizedImage{4'194'304U, 1U},
        OversizedImage{16'384U, 32'768U},
        OversizedImage{8'193U, 1U},
    })
    {
        auto const result = uf::m0_demo::ffi::writeRgbaPng(
            "oversized.png",
            image.m_width,
            image.m_height,
            {}
        );

        REQUIRE_FALSE(result.has_value());
        test_m0_demo::requireErrorKind(
            result.error(),
            uf::AutomationErrorKind::InvalidResource
        );
        CHECK(result.error().message().contains("exceed"));
    }
}
