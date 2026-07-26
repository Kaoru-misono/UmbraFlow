#include <app/canvas-math.hpp>
#include <app/workbench-app.hpp>

#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <doctest/doctest.h>

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
}
