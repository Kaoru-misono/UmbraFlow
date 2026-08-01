#include "space.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <functional>

namespace uf
{
    auto PixelRect::create(
        uint32 x,
        uint32 y,
        uint32 width,
        uint32 height
    ) -> Result<PixelRect>
    {
        if (width == 0 || height == 0)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format(
                    "empty pixel rect at ({}, {}) with size {}x{}",
                    x,
                    y,
                    width,
                    height
                )
            );
        }

        auto const right = checkedAdd(x, width);
        if (!right)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format("pixel rect x extent overflow: {} + {}", x, width)
            );
        }

        auto const bottom = checkedAdd(y, height);
        if (!bottom)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format("pixel rect y extent overflow: {} + {}", y, height)
            );
        }

        return PixelRect{x, y, width, height, *right, *bottom};
    }

    auto PixelRect::ensureWithinExtent(
        uint32 width,
        uint32 height
    ) const -> Status
    {
        if (m_right > width || m_bottom > height)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "pixel rect ({}, {}, {}, {}) outside extent {}x{}",
                    m_x,
                    m_y,
                    m_width,
                    m_height,
                    width,
                    height
                )
            );
        }

        return ok();
    }

    auto pixelRectToFrameRect(
        PixelRect const& rect
    ) -> Result<Rect<FrameSpace>>
    {
        // Compute the far edges in uint64 so a near-maximum rect cannot wrap the
        // uint32 addition and hide an out-of-range edge. Every integer in
        // [0, 2^24] inclusive is exactly representable in a 32-bit float, so
        // edges up to and including k_maxExactFrameDimension make every cast
        // below lossless.
        auto const right = uint64{rect.x()} + uint64{rect.width()};
        auto const bottom = uint64{rect.y()} + uint64{rect.height()};
        if (
            right > k_maxExactFrameDimension
            || bottom > k_maxExactFrameDimension
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "pixel rect ({}, {}, {}, {}) edges ({}, {}) exceed exact "
                    "float bound {}",
                    rect.x(),
                    rect.y(),
                    rect.width(),
                    rect.height(),
                    right,
                    bottom,
                    k_maxExactFrameDimension
                )
            );
        }

        return Rect<FrameSpace>{
            static_cast<float>(rect.x()),
            static_cast<float>(rect.y()),
            static_cast<float>(rect.width()),
            static_cast<float>(rect.height())
        };
    }

    auto pixelPointToFramePoint(
        PixelPoint point
    ) -> Result<Point<FrameSpace>>
    {
        // Every integer in [0, 2^24] inclusive is exactly representable in a
        // 32-bit float; past that bound the conversion would silently round.
        if (
            point.x() > k_maxExactFrameDimension
            || point.y() > k_maxExactFrameDimension
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "pixel point ({}, {}) exceeds exact float bound {}",
                    point.x(),
                    point.y(),
                    k_maxExactFrameDimension
                )
            );
        }

        return Point<FrameSpace>{
            static_cast<float>(point.x()),
            static_cast<float>(point.y())
        };
    }

    auto CoordinateTransform::create(
        Point<DesktopSpace> clientOrigin,
        float clientWidth,
        float clientHeight,
        uint32 frameWidth,
        uint32 frameHeight
    ) -> Result<CoordinateTransform>
    {
        auto const allFinite = (
            std::isfinite(clientOrigin.x())
            && std::isfinite(clientOrigin.y())
            && std::isfinite(clientWidth)
            && std::isfinite(clientHeight)
        );
        if (
            !allFinite
            || clientWidth <= 0.0F
            || clientHeight <= 0.0F
            || frameWidth == 0
            || frameHeight == 0
            || frameWidth > k_maxExactFrameDimension
            || frameHeight > k_maxExactFrameDimension
        )
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format(
                    "invalid transform: origin ({}, {}), client {}x{}, frame {}x{}",
                    clientOrigin.x(),
                    clientOrigin.y(),
                    clientWidth,
                    clientHeight,
                    frameWidth,
                    frameHeight
                )
            );
        }

        return CoordinateTransform{
            clientOrigin,
            clientWidth,
            clientHeight,
            frameWidth,
            frameHeight
        };
    }

    auto CoordinateTransform::frameSizeFloat() const noexcept -> std::pair<float, float>
    {
        return {
            static_cast<float>(m_frameWidth),
            static_cast<float>(m_frameHeight)
        };
    }

    auto CoordinateTransform::desktopToClient(
        Point<DesktopSpace> point
    ) const noexcept -> Point<ClientSpace>
    {
        return Point<ClientSpace>{
            point.x() - m_clientOriginX,
            point.y() - m_clientOriginY
        };
    }

    auto CoordinateTransform::clientToDesktop(
        Point<ClientSpace> point
    ) const noexcept -> Point<DesktopSpace>
    {
        return Point<DesktopSpace>{
            point.x() + m_clientOriginX,
            point.y() + m_clientOriginY
        };
    }

    auto CoordinateTransform::clientToFrame(
        Point<ClientSpace> point
    ) const noexcept -> Point<FrameSpace>
    {
        auto const [frameWidth, frameHeight] = frameSizeFloat();
        return Point<FrameSpace>{
            point.x() * frameWidth / m_clientWidth,
            point.y() * frameHeight / m_clientHeight
        };
    }

    auto CoordinateTransform::frameToClient(
        Point<FrameSpace> point
    ) const noexcept -> Point<ClientSpace>
    {
        auto const [frameWidth, frameHeight] = frameSizeFloat();
        return Point<ClientSpace>{
            point.x() * m_clientWidth / frameWidth,
            point.y() * m_clientHeight / frameHeight
        };
    }

    auto CoordinateTransform::frameToNormalized(
        Point<FrameSpace> point
    ) const noexcept -> Point<NormalizedSpace>
    {
        auto const [frameWidth, frameHeight] = frameSizeFloat();
        return Point<NormalizedSpace>{
            point.x() / frameWidth,
            point.y() / frameHeight
        };
    }

    auto CoordinateTransform::normalizedToFrame(
        Point<NormalizedSpace> point
    ) const noexcept -> Point<FrameSpace>
    {
        auto const [frameWidth, frameHeight] = frameSizeFloat();
        return Point<FrameSpace>{
            point.x() * frameWidth,
            point.y() * frameHeight
        };
    }

    auto CoordinateTransform::desktopToFrame(
        Point<DesktopSpace> point
    ) const noexcept -> Point<FrameSpace>
    {
        return clientToFrame(desktopToClient(point));
    }

    auto CoordinateTransform::frameToDesktop(
        Point<FrameSpace> point
    ) const noexcept -> Point<DesktopSpace>
    {
        return clientToDesktop(frameToClient(point));
    }

    auto CoordinateTransform::clientRectToFrame(
        Rect<ClientSpace> rect
    ) const noexcept -> Rect<FrameSpace>
    {
        auto const [frameWidth, frameHeight] = frameSizeFloat();
        auto const origin = clientToFrame(Point<ClientSpace>{rect.x(), rect.y()});
        return Rect<FrameSpace>{
            origin.x(),
            origin.y(),
            rect.width() * frameWidth / m_clientWidth,
            rect.height() * frameHeight / m_clientHeight
        };
    }

    auto CoordinateTransform::frameRectToClient(
        Rect<FrameSpace> rect
    ) const noexcept -> Rect<ClientSpace>
    {
        auto const [frameWidth, frameHeight] = frameSizeFloat();
        auto const origin = frameToClient(Point<FrameSpace>{rect.x(), rect.y()});
        return Rect<ClientSpace>{
            origin.x(),
            origin.y(),
            rect.width() * m_clientWidth / frameWidth,
            rect.height() * m_clientHeight / frameHeight
        };
    }

    auto CoordinateTransform::ensureFrameRectInBounds(
        Rect<FrameSpace> rect
    ) const -> Status
    {
        auto const [frameWidth, frameHeight] = frameSizeFloat();
        auto const allFinite = (
            std::isfinite(rect.x())
            && std::isfinite(rect.y())
            && std::isfinite(rect.width())
            && std::isfinite(rect.height())
        );
        if (!allFinite)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "non-finite frame rect ({}, {}, {}, {})",
                    rect.x(),
                    rect.y(),
                    rect.width(),
                    rect.height()
                )
            );
        }

        if (rect.isEmpty())
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format("empty frame rect {}x{}", rect.width(), rect.height())
            );
        }

        if (
            rect.x() < -k_frameBoundsEpsilon
            || rect.y() < -k_frameBoundsEpsilon
            || rect.x() + rect.width() > frameWidth + k_frameBoundsEpsilon
            || rect.y() + rect.height() > frameHeight + k_frameBoundsEpsilon
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "frame rect ({}, {}, {}, {}) outside frame {}x{}",
                    rect.x(),
                    rect.y(),
                    rect.width(),
                    rect.height(),
                    m_frameWidth,
                    m_frameHeight
                )
            );
        }

        return ok();
    }

    auto CoordinateTransform::frameRectToPixelRect(
        Rect<FrameSpace> rect
    ) const -> Result<PixelRect>
    {
        UF_TRY(ensureFrameRectInBounds(rect));

        auto const [frameWidth, frameHeight] = frameSizeFloat();
        auto const roundedX = std::floor(std::clamp(rect.x(), 0.0F, frameWidth));
        auto const roundedY = std::floor(std::clamp(rect.y(), 0.0F, frameHeight));
        auto const roundedRight = std::ceil(
            std::clamp(rect.x() + rect.width(), 0.0F, frameWidth)
        );
        auto const roundedBottom = std::ceil(
            std::clamp(rect.y() + rect.height(), 0.0F, frameHeight)
        );

        auto const x = checkedIntegralCast<uint32>(roundedX);
        auto const y = checkedIntegralCast<uint32>(roundedY);
        auto const right = checkedIntegralCast<uint32>(roundedRight);
        auto const bottom = checkedIntegralCast<uint32>(roundedBottom);
        if (!x || !y || !right || !bottom)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "validated frame rectangle could not be converted to pixel coordinates"
            );
        }

        if (*right <= *x || *bottom <= *y)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "frame rect ({}, {}, {}, {}) covers no in-bounds pixel",
                    rect.x(),
                    rect.y(),
                    rect.width(),
                    rect.height()
                )
            );
        }

        auto const width = checkedSubtract(*right, *x);
        auto const height = checkedSubtract(*bottom, *y);
        UF_CHECK(width.has_value());
        UF_CHECK(height.has_value());
        return PixelRect::create(*x, *y, *width, *height);
    }

    auto CoordinateTransform::ensureFramePointInBounds(
        Point<FrameSpace> point
    ) const -> Status
    {
        auto const [frameWidth, frameHeight] = frameSizeFloat();
        if (!std::isfinite(point.x()) || !std::isfinite(point.y()))
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format("non-finite frame point ({}, {})", point.x(), point.y())
            );
        }

        if (
            point.x() < 0.0F
            || point.y() < 0.0F
            || point.x() >= frameWidth
            || point.y() >= frameHeight
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "frame point ({}, {}) outside frame {}x{}",
                    point.x(),
                    point.y(),
                    m_frameWidth,
                    m_frameHeight
                )
            );
        }

        return ok();
    }

    auto PixelRectHash::operator()(PixelRect const& rect) const noexcept -> std::size_t
    {
        auto hash = std::hash<uint32>{}(rect.x());
        auto const values = std::array{
            rect.y(),
            rect.width(),
            rect.height()
        };
        for (auto const value : values)
        {
            hash ^= (
                std::hash<uint32>{}(value)
                + std::size_t{0x9e3779b9U}
                + (hash << 6)
                + (hash >> 2)
            );
        }

        return hash;
    }

    auto ProjectFingerprint::create(
        uint32 width,
        uint32 height,
        uint32 dpiX,
        uint32 dpiY
    ) -> Result<ProjectFingerprint>
    {
        if (width == 0 || height == 0 || dpiX == 0 || dpiY == 0)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "project fingerprint must be non-zero: {}x{} at {}x{} DPI",
                    width,
                    height,
                    dpiX,
                    dpiY
                )
            );
        }

        return ProjectFingerprint{width, height, dpiX, dpiY};
    }
}
