#include <controller/detail/capture-d3d.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace
{
    [[nodiscard]]
    auto automationKind(uf::Error const& error) -> uf::AutomationErrorKind
    {
        auto const kind = uf::automationErrorKind(error);
        REQUIRE(kind.has_value());
        return *kind;
    }

    [[nodiscard]]
    auto bytes(std::initializer_list<unsigned char> values) -> std::vector<std::byte>
    {
        auto output = std::vector<std::byte>{};
        output.reserve(values.size());
        for (auto const value : values)
        {
            output.emplace_back(static_cast<std::byte>(value));
        }
        return output;
    }
}

TEST_CASE("padded row pitch packs into a tight BGRA8 stride")
{
    constexpr auto width = std::uint32_t{2};
    constexpr auto height = std::uint32_t{2};
    constexpr auto rowPitch = std::size_t{12};
    auto source = std::vector<std::byte>(rowPitch * height);
    auto const firstRow = bytes({1, 2, 3, 4, 5, 6, 7, 8});
    auto const secondRow = bytes({9, 10, 11, 12, 13, 14, 15, 16});
    std::ranges::copy(firstRow, source.begin());
    std::ranges::copy(secondRow, source.begin() + static_cast<std::ptrdiff_t>(rowPitch));

    auto const packed = uf::controller_detail::readbackBgra8(
        source,
        rowPitch,
        width,
        height
    );

    REQUIRE(packed.has_value());
    CHECK(
        *packed
        == bytes({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16})
    );
}

TEST_CASE("BGRA8 readback accepts exact source extent boundaries")
{
    struct Case final
    {
        std::size_t m_sourceLength;
        std::size_t m_rowPitch;
    };

    for (
        auto const& testCase : std::array{
            Case{16, 8},
            Case{20, 12},
        }
    )
    {
        CAPTURE(testCase.m_sourceLength);
        CAPTURE(testCase.m_rowPitch);
        auto const source = std::vector<std::byte>(testCase.m_sourceLength);
        auto const result = uf::controller_detail::readbackBgra8(
            source,
            testCase.m_rowPitch,
            2,
            2
        );

        REQUIRE(result.has_value());
        CHECK(result->size() == 16);
    }
}

TEST_CASE("client crop maps the client area within a full-window frame")
{
    auto const crop = uf::controller_detail::ClientCropRect::create(
        {2'564, 1'487},
        {2'564, 1'487},
        {2, 45},
        {2'560, 1'440}
    );

    REQUIRE(crop.has_value());
    CHECK(crop->offsetX() == 2);
    CHECK(crop->offsetY() == 45);
    CHECK(crop->width() == 2'560);
    CHECK(crop->height() == 1'440);
    CHECK(crop->right() == 2'562);
    CHECK(crop->bottom() == 1'485);
    CHECK(crop->ensureWithinSource(2'564, 1'487).has_value());
}

TEST_CASE("zero-chrome client crop is the identity crop")
{
    auto const crop = uf::controller_detail::ClientCropRect::create(
        {800, 450},
        {800, 450},
        {0, 0},
        {800, 450}
    );

    REQUIRE(crop.has_value());
    CHECK(crop->offsetX() == 0);
    CHECK(crop->offsetY() == 0);
    CHECK(crop->width() == 800);
    CHECK(crop->height() == 450);
    CHECK(crop->right() == 800);
    CHECK(crop->bottom() == 450);
}

TEST_CASE("client crop checks extended bounds on each axis independently")
{
    struct Case final
    {
        std::int32_t m_extendedWidth;
        std::int32_t m_extendedHeight;
        bool m_accepted;
    };

    for (
        auto const& testCase : std::array{
            Case{799, 450, false},
            Case{801, 450, false},
            Case{800, 449, false},
            Case{800, 451, false},
            Case{800, 450, true},
        }
    )
    {
        CAPTURE(testCase.m_extendedWidth);
        CAPTURE(testCase.m_extendedHeight);
        auto const result = uf::controller_detail::ClientCropRect::create(
            {800, 450},
            {testCase.m_extendedWidth, testCase.m_extendedHeight},
            {0, 0},
            {800, 450}
        );

        if (testCase.m_accepted)
        {
            REQUIRE(result.has_value());
            CHECK(result->right() == 800);
            CHECK(result->bottom() == 450);
            continue;
        }

        REQUIRE_FALSE(result.has_value());
        CHECK(
            automationKind(result.error())
            == uf::AutomationErrorKind::CaptureUnavailable
        );
    }
}

TEST_CASE("nonzero client crop accepts the far edge and rejects the first outside")
{
    struct Case final
    {
        std::uint32_t m_clientWidth;
        std::uint32_t m_clientHeight;
        bool m_accepted;
    };

    for (
        auto const& testCase : std::array{
            Case{798, 405, true},
            Case{799, 405, false},
            Case{798, 406, false},
        }
    )
    {
        CAPTURE(testCase.m_clientWidth);
        CAPTURE(testCase.m_clientHeight);
        auto const result = uf::controller_detail::ClientCropRect::create(
            {800, 450},
            {800, 450},
            {2, 45},
            {testCase.m_clientWidth, testCase.m_clientHeight}
        );

        if (testCase.m_accepted)
        {
            REQUIRE(result.has_value());
            CHECK(result->right() == 800);
            CHECK(result->bottom() == 450);
            continue;
        }

        REQUIRE_FALSE(result.has_value());
        CHECK(
            automationKind(result.error())
            == uf::AutomationErrorKind::CaptureUnavailable
        );
    }
}

TEST_CASE("client crop rejects out-of-range geometry")
{
    struct Case final
    {
        std::uint32_t m_frameWidth;
        std::uint32_t m_frameHeight;
        std::int32_t m_extendedWidth;
        std::int32_t m_extendedHeight;
        std::int32_t m_offsetX;
        std::int32_t m_offsetY;
        std::uint32_t m_clientWidth;
        std::uint32_t m_clientHeight;
    };

    for (
        auto const& testCase : std::array{
            Case{2'564, 1'487, 2'560, 1'440, 2, 45, 2'560, 1'440},
            Case{2'564, 1'487, 2'564, 1'487, -1, 45, 2'560, 1'440},
            Case{2'564, 1'487, 2'564, 1'487, 2, -1, 2'560, 1'440},
            Case{2'564, 1'487, 2'564, 1'487, 2, 45, 0, 1'440},
            Case{2'564, 1'487, 2'564, 1'487, 2, 45, 2'560, 0},
            Case{800, 450, 800, 450, 0, 0, 801, 450},
            Case{800, 450, 800, 450, 0, 0, 800, 451},
            Case{800, 450, 800, 450, 5, 0, 800, 450},
        }
    )
    {
        CAPTURE(testCase.m_frameWidth);
        CAPTURE(testCase.m_frameHeight);
        CAPTURE(testCase.m_extendedWidth);
        CAPTURE(testCase.m_extendedHeight);
        CAPTURE(testCase.m_offsetX);
        CAPTURE(testCase.m_offsetY);
        CAPTURE(testCase.m_clientWidth);
        CAPTURE(testCase.m_clientHeight);
        auto const result = uf::controller_detail::ClientCropRect::create(
            {testCase.m_frameWidth, testCase.m_frameHeight},
            {testCase.m_extendedWidth, testCase.m_extendedHeight},
            {testCase.m_offsetX, testCase.m_offsetY},
            {testCase.m_clientWidth, testCase.m_clientHeight}
        );
        REQUIRE_FALSE(result.has_value());
        CHECK(
            automationKind(result.error())
            == uf::AutomationErrorKind::CaptureUnavailable
        );
    }
}

TEST_CASE("client crop checks the source texture far edge on each axis")
{
    auto const crop = uf::controller_detail::ClientCropRect::create(
        {2'564, 1'487},
        {2'564, 1'487},
        {2, 45},
        {2'560, 1'440}
    );
    REQUIRE(crop.has_value());

    struct Case final
    {
        std::uint32_t m_sourceWidth;
        std::uint32_t m_sourceHeight;
        bool m_accepted;
    };

    for (
        auto const& testCase : std::array{
            Case{2'562, 1'485, true},
            Case{2'561, 1'485, false},
            Case{2'562, 1'484, false},
        }
    )
    {
        CAPTURE(testCase.m_sourceWidth);
        CAPTURE(testCase.m_sourceHeight);
        auto const result = crop->ensureWithinSource(
            testCase.m_sourceWidth,
            testCase.m_sourceHeight
        );

        if (testCase.m_accepted)
        {
            CHECK(result.has_value());
            continue;
        }

        REQUIRE_FALSE(result.has_value());
        CHECK(
            automationKind(result.error())
            == uf::AutomationErrorKind::CaptureUnavailable
        );
    }
}

TEST_CASE("BGRA8 readback rejects inconsistent geometry")
{
    struct Case final
    {
        std::size_t m_sourceLength;
        std::size_t m_rowPitch;
        std::uint32_t m_width;
        std::uint32_t m_height;
    };

    for (
        auto const& testCase : std::array{
            Case{16, 12, 0, 2},
            Case{16, 12, 2, 0},
            Case{16, 7, 2, 2},
            Case{19, 12, 2, 2},
        }
    )
    {
        CAPTURE(testCase.m_sourceLength);
        CAPTURE(testCase.m_rowPitch);
        CAPTURE(testCase.m_width);
        CAPTURE(testCase.m_height);
        auto const source = std::vector<std::byte>(testCase.m_sourceLength);
        auto const result = uf::controller_detail::readbackBgra8(
            source,
            testCase.m_rowPitch,
            testCase.m_width,
            testCase.m_height
        );
        REQUIRE_FALSE(result.has_value());
        CHECK(
            automationKind(result.error())
            == uf::AutomationErrorKind::InternalInvariant
        );
    }
}
