#include "canvas-math.hpp"

#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        constexpr auto k_minimumZoom = 0.05F;
        constexpr auto k_maximumZoom = 64.0F;

        [[nodiscard]]
        auto clampZoom(float zoom) noexcept -> float
        {
            return std::clamp(zoom, k_minimumZoom, k_maximumZoom);
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
            .x = canvasOrigin.x + (sourceX - view.panX) * view.zoom,
            .y = canvasOrigin.y + (sourceY - view.panY) * view.zoom,
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
            .x = view.panX + (screenX - canvasOrigin.x) / view.zoom,
            .y = view.panY + (screenY - canvasOrigin.y) / view.zoom,
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
        auto const ratio   = view.zoom / clamped;

        // The screen offset of the anchor from the pan origin is
        // (source - pan) * oldZoom; holding it fixed at the new zoom means the new
        // pan keeps (source - newPan) * newZoom equal, which reduces to this.
        return CanvasView{
            .zoom = clamped,
            .panX = sourceX - (sourceX - view.panX) * ratio,
            .panY = sourceY - (sourceY - view.panY) * ratio,
        };
    }

    auto panCanvas(
        CanvasView view,
        float screenDeltaX,
        float screenDeltaY
    ) -> CanvasView
    {
        return CanvasView{
            .zoom = view.zoom,
            .panX = view.panX - screenDeltaX / view.zoom,
            .panY = view.panY - screenDeltaY / view.zoom,
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
        auto const left   = rectOrigin.x;
        auto const top    = rectOrigin.y;
        auto const right  = rectOrigin.x + rectWidth;
        auto const bottom = rectOrigin.y + rectHeight;
        auto const midX   = rectOrigin.x + rectWidth / 2.0F;
        auto const midY   = rectOrigin.y + rectHeight / 2.0F;

        auto const near = [&](float x, float y) noexcept -> bool
        {
            return std::abs(point.x - x) <= gripRadius
                && std::abs(point.y - y) <= gripRadius;
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
            point.x >= left
            && point.x <= right
            && point.y >= top
            && point.y <= bottom
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

    [[nodiscard]]
    auto rectScreenOrigin(
        CanvasView view,
        CanvasPoint canvasOrigin,
        PixelRect const& rect
    ) -> CanvasPoint
    {
        return sourceToScreen(
            view,
            canvasOrigin,
            static_cast<float>(rect.x()),
            static_cast<float>(rect.y())
        );
    }

    auto snappedScreenBounds(
        CanvasView view,
        CanvasPoint canvasOrigin,
        PixelRect const& rect
    ) -> ScreenPixelRect
    {
        auto const topLeft = sourceToScreen(
            view,
            canvasOrigin,
            static_cast<float>(rect.x()),
            static_cast<float>(rect.y())
        );
        auto const bottomRight = sourceToScreen(
            view,
            canvasOrigin,
            static_cast<float>(rect.x() + rect.width()),
            static_cast<float>(rect.y() + rect.height())
        );
        return ScreenPixelRect{
            .left   = std::round(topLeft.x),
            .top    = std::round(topLeft.y),
            .right  = std::round(bottomRight.x),
            .bottom = std::round(bottomRight.y),
        };
    }

    auto rectsUnderPoint(
        std::span<PixelRect const> rects,
        float sourceX,
        float sourceY
    ) -> std::vector<std::size_t>
    {
        auto hits = std::vector<std::size_t>{};
        for (auto index = std::size_t{0}; index < rects.size(); ++index)
        {
            auto const& rect = rects[index];
            auto const left   = static_cast<float>(rect.x());
            auto const top    = static_cast<float>(rect.y());
            auto const right  = static_cast<float>(rect.x() + rect.width());
            auto const bottom = static_cast<float>(rect.y() + rect.height());
            if (
                sourceX >= left
                && sourceX <= right
                && sourceY >= top
                && sourceY <= bottom
            )
            {
                hits.emplace_back(index);
            }
        }

        // Smallest area first so a small mark on a large region is reachable and
        // cycles ahead of it; stable so equal areas keep their input order and the
        // cycle is deterministic.
        auto const area = [&rects](std::size_t index) -> uint64
        {
            return static_cast<uint64>(rects[index].width())
                * static_cast<uint64>(rects[index].height());
        };
        std::ranges::stable_sort(
            hits,
            [&area](std::size_t lhs, std::size_t rhs)
            {
                return area(lhs) < area(rhs);
            }
        );
        return hits;
    }

    auto nextRectInCycle(
        std::span<std::size_t const> ordered,
        std::optional<std::size_t> currentIndex
    ) -> std::optional<std::size_t>
    {
        if (ordered.empty())
        {
            return std::nullopt;
        }
        if (currentIndex.has_value())
        {
            auto const found = std::ranges::find(ordered, *currentIndex);
            if (found != ordered.end())
            {
                auto const position = static_cast<std::size_t>(
                    found - ordered.begin()
                );
                return ordered[(position + 1U) % ordered.size()];
            }
        }
        return ordered.front();
    }

    auto exceedsDragThreshold(
        float screenDeltaX,
        float screenDeltaY,
        float threshold
    ) noexcept -> bool
    {
        return std::abs(screenDeltaX) > threshold
            || std::abs(screenDeltaY) > threshold;
    }

    auto rubberBandRect(
        float sourceAx,
        float sourceAy,
        float sourceBx,
        float sourceBy,
        uint32 frameWidth,
        uint32 frameHeight
    ) -> std::optional<PixelRect>
    {
        auto const extentX = static_cast<int64>(frameWidth);
        auto const extentY = static_cast<int64>(frameHeight);

        auto const left = std::clamp(
            static_cast<int64>(std::floor(std::min(sourceAx, sourceBx))),
            int64{0},
            extentX
        );
        auto const right = std::clamp(
            static_cast<int64>(std::ceil(std::max(sourceAx, sourceBx))),
            int64{0},
            extentX
        );
        auto const top = std::clamp(
            static_cast<int64>(std::floor(std::min(sourceAy, sourceBy))),
            int64{0},
            extentY
        );
        auto const bottom = std::clamp(
            static_cast<int64>(std::ceil(std::max(sourceAy, sourceBy))),
            int64{0},
            extentY
        );

        if (right - left < 1 || bottom - top < 1)
        {
            return std::nullopt;
        }
        auto rect = PixelRect::create(
            static_cast<uint32>(left),
            static_cast<uint32>(top),
            static_cast<uint32>(right - left),
            static_cast<uint32>(bottom - top)
        );
        if (!rect.has_value())
        {
            return std::nullopt;
        }
        return *rect;
    }

    auto searchRoiForDrawnTemplate(
        PixelRect templateRect,
        uint32 frameWidth,
        uint32 frameHeight
    ) -> Result<PixelRect>
    {
        auto const extentX = static_cast<int64>(frameWidth);
        auto const extentY = static_cast<int64>(frameHeight);
        auto const marginX = static_cast<int64>(templateRect.width());
        auto const marginY = static_cast<int64>(templateRect.height());

        auto const left = std::clamp(
            static_cast<int64>(templateRect.x()) - marginX,
            int64{0},
            extentX
        );
        auto const top = std::clamp(
            static_cast<int64>(templateRect.y()) - marginY,
            int64{0},
            extentY
        );
        auto const right = std::clamp(
            static_cast<int64>(templateRect.x() + templateRect.width()) + marginX,
            int64{0},
            extentX
        );
        auto const bottom = std::clamp(
            static_cast<int64>(templateRect.y() + templateRect.height()) + marginY,
            int64{0},
            extentY
        );
        return PixelRect::create(
            static_cast<uint32>(left),
            static_cast<uint32>(top),
            static_cast<uint32>(right - left),
            static_cast<uint32>(bottom - top)
        );
    }

    auto centeredCanvasView(
        float zoom,
        uint32 sourceWidth,
        uint32 sourceHeight,
        float viewportWidth,
        float viewportHeight
    ) -> CanvasView
    {
        auto const clamped = clampZoom(zoom);
        return CanvasView{
            .zoom = clamped,
            .panX = static_cast<float>(sourceWidth) / 2.0F
                - (viewportWidth / 2.0F) / clamped,
            .panY = static_cast<float>(sourceHeight) / 2.0F
                - (viewportHeight / 2.0F) / clamped,
        };
    }

    auto fitCanvasView(
        uint32 sourceWidth,
        uint32 sourceHeight,
        float viewportWidth,
        float viewportHeight
    ) -> CanvasView
    {
        if (sourceWidth == 0U || sourceHeight == 0U)
        {
            return CanvasView{};
        }
        auto const zoom = std::min(
            viewportWidth / static_cast<float>(sourceWidth),
            viewportHeight / static_cast<float>(sourceHeight)
        );
        return centeredCanvasView(
            zoom,
            sourceWidth,
            sourceHeight,
            viewportWidth,
            viewportHeight
        );
    }
}
