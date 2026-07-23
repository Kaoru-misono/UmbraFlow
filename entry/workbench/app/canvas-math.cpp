#include "canvas-math.hpp"

#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace uf::workbench
{
    namespace
    {
        constexpr auto g_minimumZoom = 0.05F;
        constexpr auto g_maximumZoom = 64.0F;

        [[nodiscard]]
        auto clampZoom(float zoom) noexcept -> float
        {
            return std::clamp(zoom, g_minimumZoom, g_maximumZoom);
        }

        // Clamps a signed edge coordinate into an inclusive integer extent so a
        // dragged edge never escapes the source image before the min-size fix-up.
        [[nodiscard]]
        auto clampEdge(int64 value, int64 extent) noexcept -> int64
        {
            return std::clamp(value, int64{0}, extent);
        }
    }

    auto sourceToScreen(
        CanvasView view,
        CanvasPoint canvasOrigin,
        float sourceX,
        float sourceY
    ) -> CanvasPoint
    {
        return CanvasPoint{
            .m_x = canvasOrigin.m_x + (sourceX - view.m_panX) * view.m_zoom,
            .m_y = canvasOrigin.m_y + (sourceY - view.m_panY) * view.m_zoom,
        };
    }

    auto screenToSource(
        CanvasView view,
        CanvasPoint canvasOrigin,
        float screenX,
        float screenY
    ) -> CanvasPoint
    {
        return CanvasPoint{
            .m_x = view.m_panX + (screenX - canvasOrigin.m_x) / view.m_zoom,
            .m_y = view.m_panY + (screenY - canvasOrigin.m_y) / view.m_zoom,
        };
    }

    auto zoomCanvasAroundSourcePoint(
        CanvasView view,
        float sourceX,
        float sourceY,
        float newZoom
    ) -> CanvasView
    {
        auto const clamped = clampZoom(newZoom);
        auto const ratio   = view.m_zoom / clamped;

        // The screen offset of the anchor from the pan origin is
        // (source - pan) * oldZoom; holding it fixed at the new zoom means the new
        // pan keeps (source - newPan) * newZoom equal, which reduces to this.
        return CanvasView{
            .m_zoom = clamped,
            .m_panX = sourceX - (sourceX - view.m_panX) * ratio,
            .m_panY = sourceY - (sourceY - view.m_panY) * ratio,
        };
    }

    auto panCanvas(
        CanvasView view,
        float screenDeltaX,
        float screenDeltaY
    ) -> CanvasView
    {
        return CanvasView{
            .m_zoom = view.m_zoom,
            .m_panX = view.m_panX - screenDeltaX / view.m_zoom,
            .m_panY = view.m_panY - screenDeltaY / view.m_zoom,
        };
    }

    auto hitTestGrip(
        CanvasPoint rectOrigin,
        float rectWidth,
        float rectHeight,
        CanvasPoint point,
        float gripRadius
    ) -> std::optional<RectGripKind>
    {
        auto const left   = rectOrigin.m_x;
        auto const top    = rectOrigin.m_y;
        auto const right  = rectOrigin.m_x + rectWidth;
        auto const bottom = rectOrigin.m_y + rectHeight;
        auto const midX   = rectOrigin.m_x + rectWidth / 2.0F;
        auto const midY   = rectOrigin.m_y + rectHeight / 2.0F;

        auto const near = [&](float x, float y) noexcept -> bool
        {
            return std::abs(point.m_x - x) <= gripRadius
                && std::abs(point.m_y - y) <= gripRadius;
        };

        // Corners first so an overlapping corner grip wins over its two edges.
        if (near(left, top)) return RectGripKind::TopLeft;
        if (near(right, top)) return RectGripKind::TopRight;
        if (near(right, bottom)) return RectGripKind::BottomRight;
        if (near(left, bottom)) return RectGripKind::BottomLeft;
        if (near(midX, top)) return RectGripKind::Top;
        if (near(right, midY)) return RectGripKind::Right;
        if (near(midX, bottom)) return RectGripKind::Bottom;
        if (near(left, midY)) return RectGripKind::Left;

        if (
            point.m_x >= left
            && point.m_x <= right
            && point.m_y >= top
            && point.m_y <= bottom
        )
        {
            return RectGripKind::Move;
        }
        return std::nullopt;
    }

    auto resizeRectByGrip(
        PixelRect rect,
        RectGripKind grip,
        int32 sourceDeltaX,
        int32 sourceDeltaY,
        uint32 sourceWidth,
        uint32 sourceHeight
    ) -> Result<PixelRect>
    {
        auto const extentX = static_cast<int64>(sourceWidth);
        auto const extentY = static_cast<int64>(sourceHeight);

        auto left   = static_cast<int64>(rect.x());
        auto top    = static_cast<int64>(rect.y());
        auto right  = static_cast<int64>(rect.x()) + rect.width();
        auto bottom = static_cast<int64>(rect.y()) + rect.height();

        auto const movesLeft = grip == RectGripKind::Left
            || grip == RectGripKind::TopLeft
            || grip == RectGripKind::BottomLeft;
        auto const movesRight = grip == RectGripKind::Right
            || grip == RectGripKind::TopRight
            || grip == RectGripKind::BottomRight;
        auto const movesTop = grip == RectGripKind::Top
            || grip == RectGripKind::TopLeft
            || grip == RectGripKind::TopRight;
        auto const movesBottom = grip == RectGripKind::Bottom
            || grip == RectGripKind::BottomLeft
            || grip == RectGripKind::BottomRight;

        if (grip == RectGripKind::Move)
        {
            auto const width  = right - left;
            auto const height = bottom - top;
            left   = clampEdge(left + sourceDeltaX, extentX - width);
            top    = clampEdge(top + sourceDeltaY, extentY - height);
            right  = left + width;
            bottom = top + height;
        }
        else
        {
            if (movesLeft) left = clampEdge(left + sourceDeltaX, extentX);
            if (movesRight) right = clampEdge(right + sourceDeltaX, extentX);
            if (movesTop) top = clampEdge(top + sourceDeltaY, extentY);
            if (movesBottom) bottom = clampEdge(bottom + sourceDeltaY, extentY);

            // Preserve at least one pixel by pushing the gripped edge back off the
            // opposite one rather than collapsing the rectangle.
            if (right <= left)
            {
                if (movesLeft) left = right - 1;
                else right = left + 1;
            }
            if (bottom <= top)
            {
                if (movesTop) top = bottom - 1;
                else bottom = top + 1;
            }
            left   = std::clamp(left, int64{0}, extentX - 1);
            top    = std::clamp(top, int64{0}, extentY - 1);
            right  = std::clamp(right, left + 1, extentX);
            bottom = std::clamp(bottom, top + 1, extentY);
        }

        return PixelRect::create(
            static_cast<uint32>(left),
            static_cast<uint32>(top),
            static_cast<uint32>(right - left),
            static_cast<uint32>(bottom - top)
        );
    }
}
