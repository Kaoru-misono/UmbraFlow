#include <domain/space.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_set>

namespace
{
    auto syntheticTransform() -> uf::CoordinateTransform
    {
        auto const result = uf::CoordinateTransform::create(
            uf::Point<uf::DesktopSpace>{100.0F, 50.0F},
            1'600.0F,
            900.0F,
            800,
            450
        );
        REQUIRE(result.has_value());
        return *result;
    }

    auto requireErrorKind(
        uf::Error const& error,
        uf::AutomationErrorKind expected
    ) -> void
    {
        auto const kind = uf::automationErrorKind(error);
        REQUIRE(kind.has_value());
        CHECK(kind == expected);
    }
}

TEST_CASE("known coordinate mappings are exact")
{
    auto const transform = syntheticTransform();
    auto const [frameWidth, frameHeight] = transform.frameSize();
    auto const desktop = uf::Point<uf::DesktopSpace>{500.0F, 250.0F};
    auto const client = transform.desktopToClient(desktop);
    auto const frame = transform.clientToFrame(client);
    auto const normalized = transform.frameToNormalized(frame);

    CHECK(frameWidth == std::uint32_t{800});
    CHECK(frameHeight == std::uint32_t{450});
    CHECK(client.x() == 400.0F);
    CHECK(client.y() == 200.0F);
    CHECK(frame.x() == 200.0F);
    CHECK(frame.y() == 100.0F);
    CHECK(normalized.x() == 0.25F);
    CHECK(normalized.y() == 100.0F / 450.0F);
}

TEST_CASE("coordinate round trips stay within tolerance")
{
    auto const transform = syntheticTransform();
    auto const original = uf::Point<uf::DesktopSpace>{123.4F, 567.8F};
    auto const viaFrame = transform.frameToDesktop(transform.desktopToFrame(original));

    CHECK(std::abs(viaFrame.x() - original.x()) < 1e-3F);
    CHECK(std::abs(viaFrame.y() - original.y()) < 1e-3F);

    auto const framePoint = uf::Point<uf::FrameSpace>{321.5F, 77.25F};
    auto const viaNormalized = transform.normalizedToFrame(
        transform.frameToNormalized(framePoint)
    );
    CHECK(std::abs(viaNormalized.x() - framePoint.x()) < 1e-3F);
    CHECK(std::abs(viaNormalized.y() - framePoint.y()) < 1e-3F);
}

TEST_CASE("rectangle conversion scales its origin and extent")
{
    auto const transform = syntheticTransform();
    auto const client = uf::Rect<uf::ClientSpace>{
        10.0F,
        20.0F,
        200.0F,
        100.0F
    };
    auto const frame = transform.clientRectToFrame(client);

    CHECK(frame.x() == 5.0F);
    CHECK(frame.y() == 10.0F);
    CHECK(frame.width() == 100.0F);
    CHECK(frame.height() == 50.0F);

    auto const roundTrip = transform.frameRectToClient(frame);
    CHECK(std::abs(roundTrip.x() - client.x()) < 1e-3F);
    CHECK(std::abs(roundTrip.width() - client.width()) < 1e-3F);
}

TEST_CASE("frame bounds accept edges and reject outside geometry")
{
    auto const transform = syntheticTransform();

    CHECK(
        transform.ensureFrameRectInBounds(
            uf::Rect<uf::FrameSpace>{0.0F, 0.0F, 800.0F, 450.0F}
        )
    );
    CHECK(
        transform.ensureFrameRectInBounds(
            uf::Rect<uf::FrameSpace>{
                0.0F,
                0.0F,
                800.0F + 1e-4F,
                450.0F
            }
        )
    );

    auto const outside = transform.ensureFrameRectInBounds(
        uf::Rect<uf::FrameSpace>{1.0F, 0.0F, 800.0F, 450.0F}
    );
    REQUIRE_FALSE(outside.has_value());
    requireErrorKind(outside.error(), uf::AutomationErrorKind::ActionRejected);

    CHECK_FALSE(
        transform.ensureFrameRectInBounds(
            uf::Rect<uf::FrameSpace>{-0.1F, 0.0F, 10.0F, 10.0F}
        )
    );
    CHECK_FALSE(
        transform.ensureFrameRectInBounds(
            uf::Rect<uf::FrameSpace>{0.0F, 0.0F, 0.0F, 10.0F}
        )
    );

    CHECK(
        transform.ensureFramePointInBounds(
            uf::Point<uf::FrameSpace>{0.0F, 0.0F}
        )
    );
    CHECK_FALSE(
        transform.ensureFramePointInBounds(
            uf::Point<uf::FrameSpace>{800.0F, 450.0F}
        )
    );
}

TEST_CASE("coordinate transform rejects invalid sizes")
{
    auto const zeroClientWidth = uf::CoordinateTransform::create(
        uf::Point<uf::DesktopSpace>{0.0F, 0.0F},
        0.0F,
        900.0F,
        800,
        450
    );
    REQUIRE_FALSE(zeroClientWidth.has_value());
    requireErrorKind(
        zeroClientWidth.error(),
        uf::AutomationErrorKind::InternalInvariant
    );

    struct FrameSizeCase final
    {
        std::uint32_t m_width;
        std::uint32_t m_height;
    };

    auto const validCases = std::array{
        FrameSizeCase{16'777'216, 450},
        FrameSizeCase{800, 16'777'216},
    };
    for (auto const& testCase : validCases)
    {
        auto const result = uf::CoordinateTransform::create(
            uf::Point<uf::DesktopSpace>{0.0F, 0.0F},
            1'600.0F,
            900.0F,
            testCase.m_width,
            testCase.m_height
        );
        REQUIRE(result.has_value());
        CHECK(result->frameSize().first == testCase.m_width);
        CHECK(result->frameSize().second == testCase.m_height);
    }

    auto const invalidCases = std::array{
        FrameSizeCase{16'777'217, 450},
        FrameSizeCase{800, 16'777'217},
    };
    for (auto const& testCase : invalidCases)
    {
        auto const result = uf::CoordinateTransform::create(
            uf::Point<uf::DesktopSpace>{0.0F, 0.0F},
            1'600.0F,
            900.0F,
            testCase.m_width,
            testCase.m_height
        );
        REQUIRE_FALSE(result.has_value());
        requireErrorKind(result.error(), uf::AutomationErrorKind::InternalInvariant);
    }
}

TEST_CASE("rectangle containment is half open")
{
    auto const rect = uf::Rect<uf::FrameSpace>{10.0F, 10.0F, 20.0F, 20.0F};

    CHECK(rect.contains(uf::Point<uf::FrameSpace>{10.0F, 10.0F}));
    CHECK(rect.contains(uf::Point<uf::FrameSpace>{29.9F, 29.9F}));
    CHECK_FALSE(rect.contains(uf::Point<uf::FrameSpace>{30.0F, 10.0F}));
    CHECK_FALSE(rect.contains(uf::Point<uf::FrameSpace>{9.9F, 10.0F}));
    CHECK((rect.center() == uf::Point<uf::FrameSpace>{20.0F, 20.0F}));
}

TEST_CASE("pixel conversion floors starts and ceils far edges")
{
    auto const result = syntheticTransform().frameRectToPixelRect(
        uf::Rect<uf::FrameSpace>{1.4F, 2.6F, 9.5F, 0.4F}
    );

    REQUIRE(result.has_value());
    CHECK(result->x() == std::uint32_t{1});
    CHECK(result->y() == std::uint32_t{2});
    CHECK(result->width() == std::uint32_t{10});
    CHECK(result->height() == std::uint32_t{1});
}

TEST_CASE("subpixel rectangles retain at least one covered pixel")
{
    auto const result = syntheticTransform().frameRectToPixelRect(
        uf::Rect<uf::FrameSpace>{3.2F, 5.6F, 0.4F, 0.4F}
    );

    REQUIRE(result.has_value());
    CHECK(result->x() == std::uint32_t{3});
    CHECK(result->y() == std::uint32_t{5});
    CHECK(result->width() == std::uint32_t{1});
    CHECK(result->height() == std::uint32_t{1});
}

TEST_CASE("pixel rectangles reject empty and overflowing extents")
{
    struct InvalidCase final
    {
        std::uint32_t m_x;
        std::uint32_t m_y;
        std::uint32_t m_width;
        std::uint32_t m_height;
    };

    auto constexpr maximum = std::numeric_limits<std::uint32_t>::max();
    auto const horizontalEdge = uf::PixelRect::create(
        maximum - 1,
        0,
        1,
        1
    );
    REQUIRE(horizontalEdge.has_value());
    CHECK(horizontalEdge->right() == maximum);

    auto const verticalEdge = uf::PixelRect::create(
        0,
        maximum - 1,
        1,
        1
    );
    REQUIRE(verticalEdge.has_value());
    CHECK(verticalEdge->bottom() == maximum);

    auto const cases = std::array{
        InvalidCase{0, 0, 0, 1},
        InvalidCase{0, 0, 1, 0},
        InvalidCase{maximum, 0, 1, 1},
        InvalidCase{0, maximum, 1, 1},
    };
    for (auto const& testCase : cases)
    {
        auto const result = uf::PixelRect::create(
            testCase.m_x,
            testCase.m_y,
            testCase.m_width,
            testCase.m_height
        );
        REQUIRE_FALSE(result.has_value());
        requireErrorKind(result.error(), uf::AutomationErrorKind::InternalInvariant);
    }
}

TEST_CASE("pixel conversion stays inside the frame")
{
    auto const transform = syntheticTransform();
    auto const edge = transform.frameRectToPixelRect(
        uf::Rect<uf::FrameSpace>{799.6F, 0.0F, 0.3F, 1.0F}
    );
    REQUIRE(edge.has_value());
    CHECK(edge->x() == std::uint32_t{799});
    CHECK(edge->width() == std::uint32_t{1});

    auto const fullExtent = transform.frameRectToPixelRect(
        uf::Rect<uf::FrameSpace>{
            0.0F,
            0.0F,
            800.0F + 1e-4F,
            450.0F
        }
    );
    REQUIRE(fullExtent.has_value());
    CHECK(fullExtent->x() == std::uint32_t{0});
    CHECK(fullExtent->y() == std::uint32_t{0});
    CHECK(fullExtent->width() == std::uint32_t{800});
    CHECK(fullExtent->height() == std::uint32_t{450});
}

TEST_CASE("pixel conversion rejects unprovable bounds")
{
    auto const transform = syntheticTransform();
    auto const rects = std::array{
        uf::Rect<uf::FrameSpace>{-0.1F, 0.0F, 1.0F, 1.0F},
        uf::Rect<uf::FrameSpace>{800.0005F, 0.0F, 0.0001F, 1.0F},
        uf::Rect<uf::FrameSpace>{799.0F, 0.0F, 2.0F, 1.0F},
        uf::Rect<uf::FrameSpace>{
            std::numeric_limits<float>::quiet_NaN(),
            0.0F,
            1.0F,
            1.0F
        },
    };

    for (auto const& rect : rects)
    {
        auto const result = transform.frameRectToPixelRect(rect);
        REQUIRE_FALSE(result.has_value());
        requireErrorKind(result.error(), uf::AutomationErrorKind::ActionRejected);
    }
}

TEST_CASE("non-finite rectangles and points fail closed")
{
    auto const transform = syntheticTransform();
    auto const badRects = std::array{
        uf::Rect<uf::FrameSpace>{
            std::numeric_limits<float>::quiet_NaN(),
            0.0F,
            10.0F,
            10.0F
        },
        uf::Rect<uf::FrameSpace>{
            0.0F,
            0.0F,
            std::numeric_limits<float>::infinity(),
            10.0F
        },
    };

    for (auto const& rect : badRects)
    {
        auto const result = transform.ensureFrameRectInBounds(rect);
        REQUIRE_FALSE(result.has_value());
        requireErrorKind(result.error(), uf::AutomationErrorKind::ActionRejected);
    }

    auto const point = transform.ensureFramePointInBounds(
        uf::Point<uf::FrameSpace>{
            std::numeric_limits<float>::quiet_NaN(),
            0.0F
        }
    );
    REQUIRE_FALSE(point.has_value());
    requireErrorKind(point.error(), uf::AutomationErrorKind::ActionRejected);
}

TEST_CASE("coordinate transform rejects non-finite construction input")
{
    auto const badSize = uf::CoordinateTransform::create(
        uf::Point<uf::DesktopSpace>{0.0F, 0.0F},
        std::numeric_limits<float>::quiet_NaN(),
        900.0F,
        800,
        450
    );
    CHECK_FALSE(badSize.has_value());

    auto const badOrigin = uf::CoordinateTransform::create(
        uf::Point<uf::DesktopSpace>{
            std::numeric_limits<float>::quiet_NaN(),
            0.0F
        },
        1'600.0F,
        900.0F,
        800,
        450
    );
    CHECK_FALSE(badOrigin.has_value());
}

TEST_CASE("pixel rectangle extent checks reject rather than clip")
{
    auto const pixelRect = uf::PixelRect::create(9, 4, 2, 2);
    REQUIRE(pixelRect.has_value());

    CHECK(pixelRect->ensureWithinExtent(11, 6));
    auto const outside = pixelRect->ensureWithinExtent(10, 6);
    REQUIRE_FALSE(outside.has_value());
    requireErrorKind(outside.error(), uf::AutomationErrorKind::ActionRejected);
}

TEST_CASE("pixel rectangles support value-based hashing")
{
    auto const pixelRect = uf::PixelRect::create(9, 4, 2, 2);
    auto const equalRect = uf::PixelRect::create(9, 4, 2, 2);
    REQUIRE(pixelRect.has_value());
    REQUIRE(equalRect.has_value());

    auto rectangles = std::unordered_set<uf::PixelRect, uf::PixelRectHash>{};
    rectangles.emplace(*pixelRect);
    CHECK(rectangles.contains(*equalRect));
}
