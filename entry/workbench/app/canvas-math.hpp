#pragma once

#include "workbench-app.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <optional>

namespace uf::workbench
{
    // Which part of a template or search rectangle a canvas gesture manipulates:
    // one of the eight resize grips, or the body for a whole-rectangle move. The
    // enum is shared by hit-testing and the edit that a released drag applies.
    enum class RectGripKind : uint8
    {
        TopLeft,
        Top,
        TopRight,
        Right,
        BottomRight,
        Bottom,
        BottomLeft,
        Left,
        Move,
    };

    // A point in the canvas' own screen space, returned instead of writing two
    // out-parameters so the coordinate pair stays a single value.
    struct CanvasPoint final
    {
        float m_x{};
        float m_y{};
    };

    // Maps a source pixel to the canvas screen point that displays it, given the
    // canvas' top-left screen origin. The view's pan is the source pixel drawn at
    // that origin and the zoom is screen-pixels per source-pixel.
    [[nodiscard]]
    auto sourceToScreen(
        CanvasView view,
        CanvasPoint canvasOrigin,
        float sourceX,
        float sourceY
    ) -> CanvasPoint;

    // The exact inverse of sourceToScreen: the source pixel under a canvas screen
    // point. Used to resolve the pixel a click or the zoom cursor addresses.
    [[nodiscard]]
    auto screenToSource(
        CanvasView view,
        CanvasPoint canvasOrigin,
        float screenX,
        float screenY
    ) -> CanvasPoint;

    // Rescales the view to newZoom while holding the given source pixel fixed on
    // screen, so a wheel zoom keeps the pixel under the cursor in place. Zoom is
    // clamped to a sane authoring range; pan is recomputed to preserve the anchor.
    [[nodiscard]]
    auto zoomCanvasAroundSourcePoint(
        CanvasView view,
        float sourceX,
        float sourceY,
        float newZoom
    ) -> CanvasView;

    // Shifts the view by a screen-space drag delta, as a middle-button pan does.
    // The delta is divided by the zoom so the content tracks the cursor one-to-one.
    [[nodiscard]]
    auto panCanvas(
        CanvasView view,
        float screenDeltaX,
        float screenDeltaY
    ) -> CanvasView;

    // The grip a canvas screen point falls on, or Move when the point is inside
    // the rectangle body but on no grip, or nullopt when it misses entirely. The
    // rectangle is given in screen coordinates and gripRadius is the grip's screen
    // half-extent; corners take priority over edges.
    [[nodiscard]]
    auto hitTestGrip(
        CanvasPoint rectOrigin,
        float rectWidth,
        float rectHeight,
        CanvasPoint point,
        float gripRadius
    ) -> std::optional<RectGripKind>;

    // Applies a grip drag, expressed as a source-pixel delta, to a rectangle and
    // returns the edited rectangle clamped inside the source extent with a minimum
    // one-pixel width and height. A move translates the whole rectangle and is
    // clamped so it stays fully inside; a resize moves only the gripped edges.
    // Fails only if the clamped rectangle is somehow rejected by PixelRect.
    [[nodiscard]]
    auto resizeRectByGrip(
        PixelRect rect,
        RectGripKind grip,
        int32 sourceDeltaX,
        int32 sourceDeltaY,
        uint32 sourceWidth,
        uint32 sourceHeight
    ) -> Result<PixelRect>;

    // Where a source-pixel rectangle's top-left corner lands on screen under the
    // current view. The rectangle's size on screen is its source size scaled by
    // the view, so an origin is all a caller needs to place it.
    [[nodiscard]]
    auto rectScreenOrigin(
        CanvasView view,
        CanvasPoint canvasOrigin,
        PixelRect const& rect
    ) -> CanvasPoint;
}
