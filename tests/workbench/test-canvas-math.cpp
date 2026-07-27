#include <app/canvas-math.hpp>
#include <app/workbench-app.hpp>

#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        [[nodiscard]]
        auto rect(uint32 x, uint32 y, uint32 width, uint32 height) -> PixelRect
        {
            auto const created = PixelRect::create(x, y, width, height);
            REQUIRE(created.has_value());
            return *created;
        }
    }

    TEST_CASE("source and screen coordinates round-trip through the view")
    {
        auto const view   = CanvasView{.zoom = 2.0F, .panX = 10.0F, .panY = 8.0F};
        auto const origin = CanvasPoint{100.0F, 120.0F};

        auto const screen = sourceToScreen(view, origin, 30.0F, 40.0F);
        CHECK(screen.x == doctest::Approx(100.0F + (30.0F - 10.0F) * 2.0F));
        CHECK(screen.y == doctest::Approx(120.0F + (40.0F - 8.0F) * 2.0F));

        auto const back = screenToSource(view, origin, screen.x, screen.y);
        CHECK(back.x == doctest::Approx(30.0F));
        CHECK(back.y == doctest::Approx(40.0F));
    }

    TEST_CASE("zooming around a source point keeps that point fixed on screen")
    {
        auto const view   = CanvasView{.zoom = 2.0F, .panX = 10.0F, .panY = 10.0F};
        auto const origin = CanvasPoint{100.0F, 100.0F};

        auto const before = sourceToScreen(view, origin, 30.0F, 40.0F);
        auto const zoomed = zoomCanvasAroundSourcePoint(view, 30.0F, 40.0F, 4.0F);
        auto const after  = sourceToScreen(zoomed, origin, 30.0F, 40.0F);

        CHECK(zoomed.zoom == doctest::Approx(4.0F));
        CHECK(after.x == doctest::Approx(before.x));
        CHECK(after.y == doctest::Approx(before.y));
    }

    TEST_CASE("panning shifts the view by the screen delta scaled by zoom")
    {
        auto const view   = CanvasView{.zoom = 2.0F, .panX = 10.0F, .panY = 10.0F};
        auto const panned = panCanvas(view, 4.0F, -6.0F);

        CHECK(panned.zoom == doctest::Approx(2.0F));
        CHECK(panned.panX == doctest::Approx(8.0F));
        CHECK(panned.panY == doctest::Approx(13.0F));
    }

    TEST_CASE("grip hit-testing distinguishes corners, edges, body, and misses")
    {
        auto const origin = CanvasPoint{0.0F, 0.0F};
        auto const width  = 100.0F;
        auto const height = 50.0F;
        auto const radius = 5.0F;

        auto const at = [&](float x, float y)
        {
            return hitTestGrip(origin, width, height, CanvasPoint{x, y}, radius);
        };

        CHECK(at(0.0F, 0.0F) == RectGripKind::TopLeft);
        CHECK(at(100.0F, 0.0F) == RectGripKind::TopRight);
        CHECK(at(100.0F, 50.0F) == RectGripKind::BottomRight);
        CHECK(at(0.0F, 50.0F) == RectGripKind::BottomLeft);
        CHECK(at(50.0F, 0.0F) == RectGripKind::Top);
        CHECK(at(100.0F, 25.0F) == RectGripKind::Right);
        CHECK(at(50.0F, 50.0F) == RectGripKind::Bottom);
        CHECK(at(0.0F, 25.0F) == RectGripKind::Left);
        CHECK(at(50.0F, 25.0F) == RectGripKind::Move);
        CHECK_FALSE(at(200.0F, 200.0F).has_value());
    }

    TEST_CASE("resizing a rectangle by a grip clamps to the source and holds a minimum size")
    {
        SUBCASE("the right grip grows the width")
        {
            auto const edited = resizeRectByGrip(
                rect(10, 10, 20, 20),
                RectGripKind::Right,
                5,
                0,
                100,
                100
            );
            REQUIRE(edited.has_value());
            CHECK(*edited == rect(10, 10, 25, 20));
        }

        SUBCASE("a grip drag past the edge clamps to the source extent")
        {
            auto const edited = resizeRectByGrip(
                rect(10, 10, 20, 20),
                RectGripKind::Right,
                1000,
                0,
                100,
                100
            );
            REQUIRE(edited.has_value());
            CHECK(*edited == rect(10, 10, 90, 20));
        }

        SUBCASE("a move keeps the size and stays inside the source")
        {
            auto const edited = resizeRectByGrip(
                rect(10, 10, 20, 20),
                RectGripKind::Move,
                1000,
                0,
                100,
                100
            );
            REQUIRE(edited.has_value());
            CHECK(*edited == rect(80, 10, 20, 20));
        }

        SUBCASE("collapsing a grip past the far edge keeps one pixel")
        {
            auto const edited = resizeRectByGrip(
                rect(10, 10, 20, 20),
                RectGripKind::Left,
                1000,
                0,
                100,
                100
            );
            REQUIRE(edited.has_value());
            CHECK(*edited == rect(29, 10, 1, 20));
        }

        SUBCASE("a corner grip moves both edges it owns")
        {
            auto const edited = resizeRectByGrip(
                rect(10, 10, 20, 20),
                RectGripKind::TopLeft,
                5,
                5,
                100,
                100
            );
            REQUIRE(edited.has_value());
            CHECK(*edited == rect(15, 15, 15, 15));
        }
    }

    TEST_CASE("snapped screen bounds round the mapped corners to whole pixels")
    {
        // A fractional zoom maps integer source pixels to fractional screen
        // coordinates; the overlay bounds round both corners so a match box lands
        // on device-pixel boundaries.
        auto const view   = CanvasView{.zoom = 1.3F, .panX = 0.0F, .panY = 0.0F};
        auto const origin = CanvasPoint{0.0F, 0.0F};

        auto const bounds = snappedScreenBounds(view, origin, rect(1, 1, 2, 2));

        // top-left source (1,1) -> 1.3 -> round 1; bottom-right (3,3) -> 3.9 ->
        // round 4.
        CHECK(bounds.left == doctest::Approx(1.0F));
        CHECK(bounds.top == doctest::Approx(1.0F));
        CHECK(bounds.right == doctest::Approx(4.0F));
        CHECK(bounds.bottom == doctest::Approx(4.0F));

        // Every edge is integral.
        CHECK(bounds.left == doctest::Approx(std::round(bounds.left)));
        CHECK(bounds.right == doctest::Approx(std::round(bounds.right)));
    }

    TEST_CASE("rects under a point come back smallest-area first")
    {
        auto const rects = std::vector<PixelRect>{
            rect(0, 0, 10, 10),
            rect(2, 2, 3, 3),
        };

        // The point is inside both, so both come back, the smaller (inner) first
        // so a mark stacked on a region is reachable and cycles ahead of it.
        auto const both = rectsUnderPoint(rects, 3.0F, 3.0F);
        REQUIRE(both.size() == 2U);
        CHECK(both.front() == 1U);
        CHECK(both.back() == 0U);

        // A point inside only the outer rect returns it alone.
        auto const outer = rectsUnderPoint(rects, 8.0F, 8.0F);
        REQUIRE(outer.size() == 1U);
        CHECK(outer.front() == 0U);

        // A point outside every rect returns nothing.
        CHECK(rectsUnderPoint(rects, 50.0F, 50.0F).empty());
    }

    TEST_CASE("cycling advances through overlapping candidates and wraps")
    {
        auto const ordered = std::vector<std::size_t>{1U, 0U};

        // Nothing selected among the hits: the first (smallest) is chosen.
        CHECK(nextRectInCycle(ordered, std::nullopt) == 1U);

        // A repeated click advances to the next, then wraps back to the first.
        CHECK(nextRectInCycle(ordered, std::optional<std::size_t>{1U}) == 0U);
        CHECK(nextRectInCycle(ordered, std::optional<std::size_t>{0U}) == 1U);

        // A selection not among the hits falls to the first candidate.
        CHECK(nextRectInCycle(ordered, std::optional<std::size_t>{7U}) == 1U);

        // No candidates, nothing to select.
        CHECK_FALSE(
            nextRectInCycle(std::vector<std::size_t>{}, std::nullopt).has_value()
        );
    }

    TEST_CASE("the drag threshold separates a click from a drag per axis")
    {
        CHECK_FALSE(exceedsDragThreshold(3.0F, 0.0F, 4.0F));
        CHECK_FALSE(exceedsDragThreshold(4.0F, 4.0F, 4.0F));
        CHECK(exceedsDragThreshold(5.0F, 0.0F, 4.0F));
        CHECK(exceedsDragThreshold(0.0F, -5.0F, 4.0F));
    }

    TEST_CASE("a rubber-band rectangle floors and ceils outward and clamps")
    {
        auto const drawn = rubberBandRect(2.4F, 3.6F, 7.1F, 9.9F, 100, 100);
        REQUIRE(drawn.has_value());
        CHECK(*drawn == rect(2, 3, 6, 7));

        // A press that never moved a whole pixel is no rectangle.
        CHECK_FALSE(rubberBandRect(5.0F, 5.0F, 5.0F, 5.0F, 100, 100).has_value());

        // A drag that starts outside the frame is clamped back inside it.
        auto const clamped = rubberBandRect(-3.0F, -3.0F, 4.0F, 4.0F, 100, 100);
        REQUIRE(clamped.has_value());
        CHECK(*clamped == rect(0, 0, 4, 4));
    }

    TEST_CASE("a drawn template seeds a search region that contains it")
    {
        auto const roi = searchRoiForDrawnTemplate(rect(2, 2, 3, 3), 8, 8);
        REQUIRE(roi.has_value());
        // Grown by the template extent on every side, clamped to the frame.
        CHECK(*roi == rect(0, 0, 8, 8));

        auto const inner = searchRoiForDrawnTemplate(rect(4, 4, 2, 2), 100, 100);
        REQUIRE(inner.has_value());
        CHECK(*inner == rect(2, 2, 6, 6));
        // The seed always encloses the template it was derived from.
        CHECK(inner->x() <= 4U);
        CHECK(inner->x() + inner->width() >= 6U);
    }

    TEST_CASE("fit centres the whole image at the largest zoom that fits")
    {
        auto const fitted = fitCanvasView(100, 50, 200.0F, 200.0F);
        CHECK(fitted.zoom == doctest::Approx(2.0F));

        // The source centre maps to the viewport centre under the fitted view.
        auto const center = sourceToScreen(
            fitted,
            CanvasPoint{0.0F, 0.0F},
            50.0F,
            25.0F
        );
        CHECK(center.x == doctest::Approx(100.0F));
        CHECK(center.y == doctest::Approx(100.0F));
    }

    TEST_CASE("a centred view holds a fixed zoom and centres the image")
    {
        auto const view = centeredCanvasView(1.0F, 100, 50, 200.0F, 200.0F);
        CHECK(view.zoom == doctest::Approx(1.0F));

        auto const center = sourceToScreen(
            view,
            CanvasPoint{0.0F, 0.0F},
            50.0F,
            25.0F
        );
        CHECK(center.x == doctest::Approx(100.0F));
        CHECK(center.y == doctest::Approx(100.0F));
    }
}
