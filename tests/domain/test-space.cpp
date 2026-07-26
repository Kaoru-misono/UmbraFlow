#include <domain/space.hpp>

#include <core/types/integer.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace uf
{
    namespace
    {
        auto syntheticTransform() -> CoordinateTransform
        {
            auto const result = CoordinateTransform::create(
                Point<DesktopSpace>{100.0F, 50.0F},
                1'600.0F,
                900.0F,
                800,
                450
            );
            REQUIRE(result.has_value());
            return *result;
        }

        auto requireErrorKind(
            Error const& error,
            AutomationErrorKind expected
        ) -> void
        {
            auto const kind = automationErrorKind(error);
            REQUIRE(kind.has_value());
            CHECK(kind == expected);
        }
    }

    TEST_CASE("known coordinate mappings are exact")
    {
        auto const transform = syntheticTransform();
        auto const [frameWidth, frameHeight] = transform.frameSize();
        auto const desktop = Point<DesktopSpace>{500.0F, 250.0F};
        auto const client = transform.desktopToClient(desktop);
        auto const frame = transform.clientToFrame(client);
        auto const normalized = transform.frameToNormalized(frame);

        CHECK(frameWidth == uint32{800});
        CHECK(frameHeight == uint32{450});
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
        auto const original = Point<DesktopSpace>{123.4F, 567.8F};
        auto const viaFrame = transform.frameToDesktop(transform.desktopToFrame(original));

        CHECK(std::abs(viaFrame.x() - original.x()) < 1e-3F);
        CHECK(std::abs(viaFrame.y() - original.y()) < 1e-3F);

        auto const framePoint = Point<FrameSpace>{321.5F, 77.25F};
        auto const viaNormalized = transform.normalizedToFrame(
            transform.frameToNormalized(framePoint)
        );
        CHECK(std::abs(viaNormalized.x() - framePoint.x()) < 1e-3F);
        CHECK(std::abs(viaNormalized.y() - framePoint.y()) < 1e-3F);
    }

    TEST_CASE("rectangle conversion scales its origin and extent")
    {
        auto const transform = syntheticTransform();
        auto const client = Rect<ClientSpace>{
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
                Rect<FrameSpace>{0.0F, 0.0F, 800.0F, 450.0F}
            )
        );
        CHECK(
            transform.ensureFrameRectInBounds(
                Rect<FrameSpace>{
                    0.0F,
                    0.0F,
                    800.0F + 1e-4F,
                    450.0F
                }
            )
        );

        auto const outside = transform.ensureFrameRectInBounds(
            Rect<FrameSpace>{1.0F, 0.0F, 800.0F, 450.0F}
        );
        REQUIRE_FALSE(outside.has_value());
        requireErrorKind(outside.error(), AutomationErrorKind::ActionRejected);

        CHECK_FALSE(
            transform.ensureFrameRectInBounds(
                Rect<FrameSpace>{-0.1F, 0.0F, 10.0F, 10.0F}
            )
        );
        CHECK_FALSE(
            transform.ensureFrameRectInBounds(
                Rect<FrameSpace>{0.0F, 0.0F, 0.0F, 10.0F}
            )
        );

        CHECK(
            transform.ensureFramePointInBounds(
                Point<FrameSpace>{0.0F, 0.0F}
            )
        );
        CHECK_FALSE(
            transform.ensureFramePointInBounds(
                Point<FrameSpace>{800.0F, 450.0F}
            )
        );
    }

    TEST_CASE("coordinate transform rejects invalid sizes")
    {
        auto const zeroClientWidth = CoordinateTransform::create(
            Point<DesktopSpace>{0.0F, 0.0F},
            0.0F,
            900.0F,
            800,
            450
        );
        REQUIRE_FALSE(zeroClientWidth.has_value());
        requireErrorKind(
            zeroClientWidth.error(),
            AutomationErrorKind::InternalInvariant
        );

        struct FrameSizeCase final
        {
            uint32 width{};
            uint32 height{};
        };

        auto const validCases = std::array{
            FrameSizeCase{16'777'216, 450},
            FrameSizeCase{800, 16'777'216},
        };
        for (auto const& testCase : validCases)
        {
            auto const result = CoordinateTransform::create(
                Point<DesktopSpace>{0.0F, 0.0F},
                1'600.0F,
                900.0F,
                testCase.width,
                testCase.height
            );
            REQUIRE(result.has_value());
            CHECK(result->frameSize().first == testCase.width);
            CHECK(result->frameSize().second == testCase.height);
        }

        auto const invalidCases = std::array{
            FrameSizeCase{16'777'217, 450},
            FrameSizeCase{800, 16'777'217},
        };
        for (auto const& testCase : invalidCases)
        {
            auto const result = CoordinateTransform::create(
                Point<DesktopSpace>{0.0F, 0.0F},
                1'600.0F,
                900.0F,
                testCase.width,
                testCase.height
            );
            REQUIRE_FALSE(result.has_value());
            requireErrorKind(result.error(), AutomationErrorKind::InternalInvariant);
        }
    }

    TEST_CASE("rectangle containment is half open")
    {
        auto const rect = Rect<FrameSpace>{10.0F, 10.0F, 20.0F, 20.0F};

        CHECK(rect.contains(Point<FrameSpace>{10.0F, 10.0F}));
        CHECK(rect.contains(Point<FrameSpace>{29.9F, 29.9F}));
        CHECK_FALSE(rect.contains(Point<FrameSpace>{30.0F, 10.0F}));
        CHECK_FALSE(rect.contains(Point<FrameSpace>{9.9F, 10.0F}));
        CHECK((rect.center() == Point<FrameSpace>{20.0F, 20.0F}));
    }

    TEST_CASE("pixel conversion floors starts and ceils far edges")
    {
        auto const result = syntheticTransform().frameRectToPixelRect(
            Rect<FrameSpace>{1.4F, 2.6F, 9.5F, 0.4F}
        );

        REQUIRE(result.has_value());
        CHECK(result->x() == uint32{1});
        CHECK(result->y() == uint32{2});
        CHECK(result->width() == uint32{10});
        CHECK(result->height() == uint32{1});
    }

    TEST_CASE("subpixel rectangles retain at least one covered pixel")
    {
        auto const result = syntheticTransform().frameRectToPixelRect(
            Rect<FrameSpace>{3.2F, 5.6F, 0.4F, 0.4F}
        );

        REQUIRE(result.has_value());
        CHECK(result->x() == uint32{3});
        CHECK(result->y() == uint32{5});
        CHECK(result->width() == uint32{1});
        CHECK(result->height() == uint32{1});
    }

    TEST_CASE("pixel rectangles reject empty and overflowing extents")
    {
        struct InvalidCase final
        {
            uint32 x{};
            uint32 y{};
            uint32 width{};
            uint32 height{};
        };

        auto constexpr maximum = std::numeric_limits<uint32>::max();
        auto const horizontalEdge = PixelRect::create(
            maximum - 1,
            0,
            1,
            1
        );
        REQUIRE(horizontalEdge.has_value());
        CHECK(horizontalEdge->right() == maximum);

        auto const verticalEdge = PixelRect::create(
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
            auto const result = PixelRect::create(
                testCase.x,
                testCase.y,
                testCase.width,
                testCase.height
            );
            REQUIRE_FALSE(result.has_value());
            requireErrorKind(result.error(), AutomationErrorKind::InternalInvariant);
        }
    }

    TEST_CASE("pixel conversion stays inside the frame")
    {
        auto const transform = syntheticTransform();
        auto const edge = transform.frameRectToPixelRect(
            Rect<FrameSpace>{799.6F, 0.0F, 0.3F, 1.0F}
        );
        REQUIRE(edge.has_value());
        CHECK(edge->x() == uint32{799});
        CHECK(edge->width() == uint32{1});

        auto const fullExtent = transform.frameRectToPixelRect(
            Rect<FrameSpace>{
                0.0F,
                0.0F,
                800.0F + 1e-4F,
                450.0F
            }
        );
        REQUIRE(fullExtent.has_value());
        CHECK(fullExtent->x() == uint32{0});
        CHECK(fullExtent->y() == uint32{0});
        CHECK(fullExtent->width() == uint32{800});
        CHECK(fullExtent->height() == uint32{450});
    }

    TEST_CASE("pixel conversion rejects unprovable bounds")
    {
        auto const transform = syntheticTransform();
        auto const rects = std::array{
            Rect<FrameSpace>{-0.1F, 0.0F, 1.0F, 1.0F},
            Rect<FrameSpace>{800.0005F, 0.0F, 0.0001F, 1.0F},
            Rect<FrameSpace>{799.0F, 0.0F, 2.0F, 1.0F},
            Rect<FrameSpace>{
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
            requireErrorKind(result.error(), AutomationErrorKind::ActionRejected);
        }
    }

    TEST_CASE("non-finite rectangles and points fail closed")
    {
        auto const transform = syntheticTransform();
        auto const badRects = std::array{
            Rect<FrameSpace>{
                std::numeric_limits<float>::quiet_NaN(),
                0.0F,
                10.0F,
                10.0F
            },
            Rect<FrameSpace>{
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
            requireErrorKind(result.error(), AutomationErrorKind::ActionRejected);
        }

        auto const point = transform.ensureFramePointInBounds(
            Point<FrameSpace>{
                std::numeric_limits<float>::quiet_NaN(),
                0.0F
            }
        );
        REQUIRE_FALSE(point.has_value());
        requireErrorKind(point.error(), AutomationErrorKind::ActionRejected);
    }

    TEST_CASE("coordinate transform rejects non-finite construction input")
    {
        auto const badSize = CoordinateTransform::create(
            Point<DesktopSpace>{0.0F, 0.0F},
            std::numeric_limits<float>::quiet_NaN(),
            900.0F,
            800,
            450
        );
        CHECK_FALSE(badSize.has_value());

        auto const badOrigin = CoordinateTransform::create(
            Point<DesktopSpace>{
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
        auto const pixelRect = PixelRect::create(9, 4, 2, 2);
        REQUIRE(pixelRect.has_value());

        CHECK(pixelRect->ensureWithinExtent(11, 6));
        auto const outside = pixelRect->ensureWithinExtent(10, 6);
        REQUIRE_FALSE(outside.has_value());
        requireErrorKind(outside.error(), AutomationErrorKind::ActionRejected);
    }

    TEST_CASE("pixel rectangles support value-based hashing")
    {
        auto const pixelRect = PixelRect::create(9, 4, 2, 2);
        auto const equalRect = PixelRect::create(9, 4, 2, 2);
        REQUIRE(pixelRect.has_value());
        REQUIRE(equalRect.has_value());

        auto rectangles = std::unordered_set<PixelRect, PixelRectHash>{};
        rectangles.emplace(*pixelRect);
        CHECK(rectangles.contains(*equalRect));
    }

    TEST_CASE("pixel rect to frame rect is exact up to the float edge bound")
    {
        auto const atOrigin = PixelRect::create(0, 0, 1, 1);
        REQUIRE(atOrigin.has_value());
        auto const originFrame = pixelRectToFrameRect(*atOrigin);
        REQUIRE(originFrame.has_value());
        CHECK((*originFrame == Rect<FrameSpace>{0.0F, 0.0F, 1.0F, 1.0F}));

        auto const offset = PixelRect::create(1, 1, 2, 3);
        REQUIRE(offset.has_value());
        auto const offsetFrame = pixelRectToFrameRect(*offset);
        REQUIRE(offsetFrame.has_value());
        CHECK((*offsetFrame == Rect<FrameSpace>{1.0F, 1.0F, 2.0F, 3.0F}));

        auto const edgeAtBound = PixelRect::create(
            k_maxExactFrameDimension - 1,
            k_maxExactFrameDimension - 1,
            1,
            1
        );
        REQUIRE(edgeAtBound.has_value());
        CHECK(pixelRectToFrameRect(*edgeAtBound).has_value());

        auto const horizontalBeyond = PixelRect::create(
            k_maxExactFrameDimension,
            0,
            1,
            1
        );
        REQUIRE(horizontalBeyond.has_value());
        auto const horizontal = pixelRectToFrameRect(*horizontalBeyond);
        REQUIRE_FALSE(horizontal.has_value());
        requireErrorKind(horizontal.error(), AutomationErrorKind::InvalidResource);

        auto const verticalBeyond = PixelRect::create(
            0,
            k_maxExactFrameDimension,
            1,
            1
        );
        REQUIRE(verticalBeyond.has_value());
        auto const vertical = pixelRectToFrameRect(*verticalBeyond);
        REQUIRE_FALSE(vertical.has_value());
        requireErrorKind(vertical.error(), AutomationErrorKind::InvalidResource);
    }

    TEST_CASE("pixel point to frame point is exact up to the float bound")
    {
        auto const atOrigin = pixelPointToFramePoint(PixelPoint{0, 0});
        REQUIRE(atOrigin.has_value());
        CHECK((*atOrigin == Point<FrameSpace>{0.0F, 0.0F}));

        auto const offset = pixelPointToFramePoint(PixelPoint{1, 1});
        REQUIRE(offset.has_value());
        CHECK((*offset == Point<FrameSpace>{1.0F, 1.0F}));

        auto const atBound = pixelPointToFramePoint(
            PixelPoint{k_maxExactFrameDimension, k_maxExactFrameDimension}
        );
        REQUIRE(atBound.has_value());
        auto const boundEdge = static_cast<float>(k_maxExactFrameDimension);
        CHECK((*atBound == Point<FrameSpace>{boundEdge, boundEdge}));

        auto const beyondX = pixelPointToFramePoint(
            PixelPoint{k_maxExactFrameDimension + 1, 0}
        );
        REQUIRE_FALSE(beyondX.has_value());
        requireErrorKind(beyondX.error(), AutomationErrorKind::InvalidResource);

        auto const beyondY = pixelPointToFramePoint(
            PixelPoint{0, k_maxExactFrameDimension + 1}
        );
        REQUIRE_FALSE(beyondY.has_value());
        requireErrorKind(beyondY.error(), AutomationErrorKind::InvalidResource);
    }

    TEST_CASE("integer pixel rect round trips through the frame rect bridge")
    {
        auto const identity = CoordinateTransform::create(
            Point<DesktopSpace>{0.0F, 0.0F},
            800.0F,
            450.0F,
            800,
            450
        );
        REQUIRE(identity.has_value());

        auto const pixelRect = PixelRect::create(10, 20, 30, 40);
        REQUIRE(pixelRect.has_value());

        auto const frameRect = pixelRectToFrameRect(*pixelRect);
        REQUIRE(frameRect.has_value());

        auto const roundTrip = identity->frameRectToPixelRect(*frameRect);
        REQUIRE(roundTrip.has_value());
        CHECK((*roundTrip == *pixelRect));
    }
}
