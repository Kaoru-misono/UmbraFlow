#include <domain/frame.hpp>

#include <core/safety/checked-access.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>
#include <vector>

namespace
{
    auto frameTransform() -> uf::CoordinateTransform
    {
        auto const result = uf::CoordinateTransform::create(
            uf::Point<uf::DesktopSpace>{0.0F, 0.0F},
            4.0F,
            2.0F,
            4,
            2
        );
        REQUIRE(result.has_value());
        return *result;
    }

    auto pixelBuffer(std::size_t length) -> std::shared_ptr<uf::FrameBuffer const>
    {
        return std::make_shared<uf::FrameBuffer>(
            std::vector<std::byte>(length)
        );
    }

    [[nodiscard]]
    auto makeFrame(
        std::uint32_t width,
        std::uint32_t height,
        std::size_t stride,
        std::size_t length
    ) -> uf::Result<uf::Frame>
    {
        return uf::Frame::create(
            uf::FrameId{std::uint64_t{1}},
            uf::SessionId{std::uint64_t{1}},
            uf::TargetGeneration{},
            uf::MonotonicInstant::fromTimePoint(
                uf::MonotonicInstant::TimePoint{}
            ),
            width,
            height,
            stride,
            uf::PixelFormat::Bgra8,
            pixelBuffer(length),
            frameTransform()
        );
    }

    auto requireInternalInvariant(uf::Error const& error) -> void
    {
        auto const kind = uf::automationErrorKind(error);
        REQUIRE(kind.has_value());
        CHECK(kind.value() == uf::AutomationErrorKind::InternalInvariant);
    }
}

TEST_CASE("valid frames preserve immutable shared pixels")
{
    static_assert(!std::is_copy_assignable_v<uf::FrameBuffer>);
    static_assert(!std::is_move_assignable_v<uf::FrameBuffer>);

    auto const owner = std::make_shared<uf::FrameBuffer>(
        std::vector<std::byte>(32, std::byte{7})
    );
    auto const result = uf::Frame::create(
        uf::FrameId{std::uint64_t{1}},
        uf::SessionId{std::uint64_t{1}},
        uf::TargetGeneration{},
        uf::MonotonicInstant::fromTimePoint(
            uf::MonotonicInstant::TimePoint{}
        ),
        4,
        2,
        16,
        uf::PixelFormat::Bgra8,
        owner,
        frameTransform()
    );

    REQUIRE(result.has_value());
    auto const framePixels = result->pixels();
    CHECK(framePixels.get() == owner.get());
    CHECK(framePixels->size() == std::size_t{32});
    CHECK(uf::checkedAt(framePixels->bytes(), 0) == std::byte{7});
    CHECK(uf::checkedAt(owner->bytes(), 0) == std::byte{7});
    CHECK(uf::bytesPerPixel(result->pixelFormat()) == std::size_t{4});
    CHECK(uf::bytesPerPixel(uf::PixelFormat::Gray8) == std::size_t{1});
}

TEST_CASE("frame stride accepts the first valid value and padding")
{
    auto const belowMinimum = makeFrame(4, 2, 15, 30);
    REQUIRE_FALSE(belowMinimum.has_value());
    requireInternalInvariant(belowMinimum.error());

    CHECK(makeFrame(4, 2, 16, 32));
    CHECK(makeFrame(4, 2, 17, 34));
}

TEST_CASE("frame buffer length accepts the exact minimum and padding")
{
    auto const belowMinimum = makeFrame(4, 2, 16, 31);
    REQUIRE_FALSE(belowMinimum.has_value());
    requireInternalInvariant(belowMinimum.error());

    CHECK(makeFrame(4, 2, 16, 32));
    CHECK(makeFrame(4, 2, 16, 33));
}

TEST_CASE("zero frame dimensions are rejected")
{
    struct InvalidCase final
    {
        std::uint32_t m_width;
        std::uint32_t m_height;
        std::size_t m_stride;
        std::size_t m_length;
    };

    auto const cases = std::array{
        InvalidCase{0, 2, 16, 32},
        InvalidCase{4, 0, 16, 0},
    };
    for (auto const& testCase : cases)
    {
        auto const result = makeFrame(
            testCase.m_width,
            testCase.m_height,
            testCase.m_stride,
            testCase.m_length
        );
        REQUIRE_FALSE(result.has_value());
        requireInternalInvariant(result.error());
    }
}

TEST_CASE("buffer geometry rejects size overflow")
{
    auto constexpr maximum = std::numeric_limits<std::size_t>::max();
    auto constexpr lastValidStride = maximum / 2;
    CHECK(
        uf::validateBufferGeometry(
            1,
            2,
            lastValidStride,
            1,
            maximum - 1
        )
    );

    auto const result = uf::validateBufferGeometry(
        1,
        2,
        lastValidStride + 1,
        1,
        maximum
    );

    REQUIRE_FALSE(result.has_value());
    requireInternalInvariant(result.error());
}

TEST_CASE("frame buffers expose read-only bytes")
{
    auto const buffer = uf::FrameBuffer{
        std::vector<std::byte>{std::byte{1}, std::byte{2}, std::byte{3}}
    };
    using Bytes = decltype(buffer.bytes());
    static_assert(std::is_const_v<typename Bytes::element_type>);

    REQUIRE(buffer.bytes().size() == std::size_t{3});
    CHECK(uf::checkedAt(buffer.bytes(), 0) == std::byte{1});
    CHECK(uf::checkedAt(buffer.bytes(), 1) == std::byte{2});
    CHECK(uf::checkedAt(buffer.bytes(), 2) == std::byte{3});
}

TEST_CASE("frame transform rejects separate width and height mismatches")
{
    struct MismatchCase final
    {
        std::uint32_t m_width;
        std::uint32_t m_height;
    };

    auto const cases = std::array{
        MismatchCase{5, 2},
        MismatchCase{4, 3},
    };
    for (auto const& testCase : cases)
    {
        auto const transform = uf::CoordinateTransform::create(
            uf::Point<uf::DesktopSpace>{0.0F, 0.0F},
            4.0F,
            2.0F,
            testCase.m_width,
            testCase.m_height
        );
        REQUIRE(transform.has_value());

        auto const result = uf::Frame::create(
            uf::FrameId{std::uint64_t{1}},
            uf::SessionId{std::uint64_t{1}},
            uf::TargetGeneration{},
            uf::MonotonicInstant::fromTimePoint(
                uf::MonotonicInstant::TimePoint{}
            ),
            4,
            2,
            16,
            uf::PixelFormat::Bgra8,
            pixelBuffer(32),
            *transform
        );

        REQUIRE_FALSE(result.has_value());
        requireInternalInvariant(result.error());
    }
}

TEST_CASE("frame construction rejects a null pixel owner")
{
    auto const result = uf::Frame::create(
        uf::FrameId{std::uint64_t{1}},
        uf::SessionId{std::uint64_t{1}},
        uf::TargetGeneration{},
        uf::MonotonicInstant::fromTimePoint(
            uf::MonotonicInstant::TimePoint{}
        ),
        4,
        2,
        16,
        uf::PixelFormat::Bgra8,
        std::shared_ptr<uf::FrameBuffer const>{},
        frameTransform()
    );

    REQUIRE_FALSE(result.has_value());
    requireInternalInvariant(result.error());
}
