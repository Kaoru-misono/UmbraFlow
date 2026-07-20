#include "space.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>

namespace uf
{
    auto PixelRect::create(
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t width,
        std::uint32_t height
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
        std::uint32_t width,
        std::uint32_t height
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

    auto CoordinateTransform::create(
        Point<DesktopSpace> clientOrigin,
        float clientWidth,
        float clientHeight,
        std::uint32_t frameWidth,
        std::uint32_t frameHeight
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
            || frameWidth > s_maxExactFrameDimension
            || frameHeight > s_maxExactFrameDimension
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
            rect.x() < -s_frameBoundsEpsilon
            || rect.y() < -s_frameBoundsEpsilon
            || rect.x() + rect.width() > frameWidth + s_frameBoundsEpsilon
            || rect.y() + rect.height() > frameHeight + s_frameBoundsEpsilon
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

        auto const x = checkedIntegralCast<std::uint32_t>(roundedX);
        auto const y = checkedIntegralCast<std::uint32_t>(roundedY);
        auto const right = checkedIntegralCast<std::uint32_t>(roundedRight);
        auto const bottom = checkedIntegralCast<std::uint32_t>(roundedBottom);
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
        auto hash = std::hash<std::uint32_t>{}(rect.x());
        auto const values = std::array{
            rect.y(),
            rect.width(),
            rect.height()
        };
        for (auto const value : values)
        {
            hash ^= (
                std::hash<std::uint32_t>{}(value)
                + std::size_t{0x9e3779b9U}
                + (hash << 6)
                + (hash >> 2)
            );
        }

        return hash;
    }
}
