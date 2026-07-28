#include <domain/frame.hpp>

#include <core/safety/checked-access.hpp>
#include <core/types/integer.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <type_traits>
#include <vector>

namespace uf
{
    namespace
    {
        auto frameTransform() -> CoordinateTransform
        {
            auto const result = CoordinateTransform::create(
                Point<DesktopSpace>{0.0F, 0.0F},
                4.0F,
                2.0F,
                4,
                2
            );
            REQUIRE(result.has_value());
            return *result;
        }

        auto pixelBuffer(std::size_t length) -> std::shared_ptr<FrameBuffer const>
        {
            return std::make_shared<FrameBuffer>(
                std::vector<std::byte>(length)
            );
        }

        [[nodiscard]]
        auto makeFrame(
            uint32 width,
            uint32 height,
            std::size_t stride,
            std::size_t length
        ) -> Result<Frame>
        {
            return Frame::create(
                FrameId{uint64{1}},
                CaptureSessionId{uint64{1}},
                TargetGeneration{},
                MonotonicInstant::fromTimePoint(
                    MonotonicInstant::TimePoint{}
                ),
                width,
                height,
                stride,
                PixelFormat::Bgra8,
                pixelBuffer(length),
                frameTransform()
            );
        }

        auto requireInternalInvariant(Error const& error) -> void
        {
            auto const kind = automationErrorKind(error);
            REQUIRE(kind.has_value());
            CHECK(kind == AutomationErrorKind::InternalInvariant);
        }
    }

    TEST_CASE("valid frames preserve immutable shared pixels")
    {
        static_assert(!std::is_copy_assignable_v<FrameBuffer>);
        static_assert(!std::is_move_assignable_v<FrameBuffer>);

        auto const owner = std::make_shared<FrameBuffer>(
            std::vector<std::byte>(32, std::byte{7})
        );
        auto const result = Frame::create(
            FrameId{uint64{1}},
            CaptureSessionId{uint64{1}},
            TargetGeneration{},
            MonotonicInstant::fromTimePoint(
                MonotonicInstant::TimePoint{}
            ),
            4,
            2,
            16,
            PixelFormat::Bgra8,
            owner,
            frameTransform()
        );

        REQUIRE(result.has_value());
        auto const framePixels = result->pixels();
        CHECK(framePixels.get() == owner.get());
        CHECK(framePixels->size() == std::size_t{32});
        CHECK(checkedAt(framePixels->bytes(), 0) == std::byte{7});
        CHECK(checkedAt(owner->bytes(), 0) == std::byte{7});
        CHECK(bytesPerPixel(result->pixelFormat()) == std::size_t{4});
        CHECK(bytesPerPixel(PixelFormat::Gray8) == std::size_t{1});
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
            uint32 width{};
            uint32 height{};
            std::size_t stride{};
            std::size_t length{};
        };

        auto const cases = std::array{
            InvalidCase{0, 2, 16, 32},
            InvalidCase{4, 0, 16, 0},
        };
        for (auto const& testCase : cases)
        {
            auto const result = makeFrame(
                testCase.width,
                testCase.height,
                testCase.stride,
                testCase.length
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
            validateBufferGeometry(
                1,
                2,
                lastValidStride,
                1,
                maximum - 1
            )
        );

        auto const result = validateBufferGeometry(
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
        auto const buffer = FrameBuffer{
            std::vector<std::byte>{std::byte{1}, std::byte{2}, std::byte{3}}
        };
        using Bytes = decltype(buffer.bytes());
        static_assert(std::is_const_v<typename Bytes::element_type>);

        REQUIRE(buffer.bytes().size() == std::size_t{3});
        CHECK(checkedAt(buffer.bytes(), 0) == std::byte{1});
        CHECK(checkedAt(buffer.bytes(), 1) == std::byte{2});
        CHECK(checkedAt(buffer.bytes(), 2) == std::byte{3});
    }

    TEST_CASE("frame transform rejects separate width and height mismatches")
    {
        struct MismatchCase final
        {
            uint32 width{};
            uint32 height{};
        };

        auto const cases = std::array{
            MismatchCase{5, 2},
            MismatchCase{4, 3},
        };
        for (auto const& testCase : cases)
        {
            auto const transform = CoordinateTransform::create(
                Point<DesktopSpace>{0.0F, 0.0F},
                4.0F,
                2.0F,
                testCase.width,
                testCase.height
            );
            REQUIRE(transform.has_value());

            auto const result = Frame::create(
                FrameId{uint64{1}},
                CaptureSessionId{uint64{1}},
                TargetGeneration{},
                MonotonicInstant::fromTimePoint(
                    MonotonicInstant::TimePoint{}
                ),
                4,
                2,
                16,
                PixelFormat::Bgra8,
                pixelBuffer(32),
                *transform
            );

            REQUIRE_FALSE(result.has_value());
            requireInternalInvariant(result.error());
        }
    }

    TEST_CASE("frame construction rejects a null pixel owner")
    {
        auto const result = Frame::create(
            FrameId{uint64{1}},
            CaptureSessionId{uint64{1}},
            TargetGeneration{},
            MonotonicInstant::fromTimePoint(
                MonotonicInstant::TimePoint{}
            ),
            4,
            2,
            16,
            PixelFormat::Bgra8,
            std::shared_ptr<FrameBuffer const>{},
            frameTransform()
        );

        REQUIRE_FALSE(result.has_value());
        requireInternalInvariant(result.error());
    }
}
