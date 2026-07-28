#pragma once

#include "workbench-app.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

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
        float x{};
        float y{};
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

    // A screen-space rectangle whose four edges have been rounded to whole
    // screen pixels: the device-pixel-aligned bounds a source rect maps to under
    // the view. An evidence overlay snaps to this so a match box drawn at a
    // fractional zoom lands on crisp integer boundaries rather than smeared
    // across two device pixels.
    struct ScreenPixelRect final
    {
        float left{};
        float top{};
        float right{};
        float bottom{};
    };

    [[nodiscard]]
    auto snappedScreenBounds(
        CanvasView view,
        CanvasPoint canvasOrigin,
        PixelRect const& rect
    ) -> ScreenPixelRect;

    // The indices of every rectangle that contains a source-space point, ordered
    // smallest-area first so a small mark stacked on a large one is reachable and
    // cycles ahead of it. Equal-area rectangles keep their input order. This is
    // the canvas' click hit-test: the caller passes each drawn member's template
    // rectangle and the source pixel under the cursor.
    [[nodiscard]]
    auto rectsUnderPoint(
        std::span<PixelRect const> rects,
        float sourceX,
        float sourceY
    ) -> std::vector<std::size_t>;

    // The next candidate to select given the ordered hits under the cursor and
    // the index of the currently selected rectangle. When the current selection
    // is among the hits, the one after it wraps around; otherwise, or when
    // nothing is selected, the first (smallest) hit. Nothing when there are no
    // hits. This is what makes repeated clicks on overlapping rectangles cycle.
    [[nodiscard]]
    auto nextRectInCycle(
        std::span<std::size_t const> ordered,
        std::optional<std::size_t> currentIndex
    ) -> std::optional<std::size_t>;

    // Whether a press-to-drag has moved far enough on screen to be a drag rather
    // than a click. Compared per-axis against the threshold so a mostly-vertical
    // or mostly-horizontal drag crosses it as readily as a diagonal one.
    [[nodiscard]]
    auto exceedsDragThreshold(
        float screenDeltaX,
        float screenDeltaY,
        float threshold
    ) noexcept -> bool;

    // The integer source rectangle a rubber-band drag between two source-space
    // points covers, clamped inside the frame. Nothing when the two points span
    // no whole pixel in either axis (a click, or a zero-width drag). The corners
    // are floored and ceiled outward so a rectangle the author drags always
    // encloses the pixels under the gesture.
    [[nodiscard]]
    auto rubberBandRect(
        float sourceAx,
        float sourceAy,
        float sourceBx,
        float sourceBy,
        uint32 frameWidth,
        uint32 frameHeight
    ) -> std::optional<PixelRect>;

    // The initial per-page search region for a freshly drawn member: its template
    // grown by the template's own extent on every side and clamped to the frame,
    // so the runtime first looks for the mark near where it was drawn rather than
    // across the whole frame. Always contains the template, since it only grows
    // outward from a template already inside the frame. Fails only if the clamped
    // rectangle is somehow rejected by PixelRect.
    [[nodiscard]]
    auto searchRoiForDrawnTemplate(
        PixelRect templateRect,
        uint32 frameWidth,
        uint32 frameHeight
    ) -> Result<PixelRect>;

    // Fits the whole source image inside a viewport: the largest zoom at which
    // both dimensions fit, with the pan centring the image. The Fit button's
    // math, kept pure so it is tested without a canvas.
    [[nodiscard]]
    auto fitCanvasView(
        uint32 sourceWidth,
        uint32 sourceHeight,
        float viewportWidth,
        float viewportHeight
    ) -> CanvasView;

    // The view at a fixed zoom with the source image centred in the viewport, as
    // the 100% button produces at zoom 1. Shares the centring math with
    // fitCanvasView.
    [[nodiscard]]
    auto centeredCanvasView(
        float zoom,
        uint32 sourceWidth,
        uint32 sourceHeight,
        float viewportWidth,
        float viewportHeight
    ) -> CanvasView;
}
