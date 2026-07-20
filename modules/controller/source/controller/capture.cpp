#include "capture.hpp"

#include <domain/error.hpp>

#include <cmath>
#include <format>

namespace uf
{
    auto WgcCaptureOptions::create(
        MonotonicInstant::Duration captureStallTimeout,
        bool requireBorderless
    ) -> Result<WgcCaptureOptions>
    {
        if (captureStallTimeout <= MonotonicInstant::Duration::zero())
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "captureStallTimeout must be positive"
            );
        }

        return WgcCaptureOptions{captureStallTimeout, requireBorderless};
    }

    auto ClientGeometry::create(
        Point<DesktopSpace> origin,
        float width,
        float height
    ) -> Result<ClientGeometry>
    {
        if (
            !std::isfinite(origin.x())
            || !std::isfinite(origin.y())
            || !std::isfinite(width)
            || !std::isfinite(height)
            || width <= 0.0F
            || height <= 0.0F
        )
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format(
                    "invalid client geometry: origin ({}, {}), size {}x{}",
                    origin.x(),
                    origin.y(),
                    width,
                    height
                )
            );
        }

        return ClientGeometry{origin, width, height};
    }

    auto ClientGeometry::transformFor(
        std::uint32_t frameWidth,
        std::uint32_t frameHeight
    ) const -> Result<CoordinateTransform>
    {
        return CoordinateTransform::create(
            m_origin,
            m_width,
            m_height,
            frameWidth,
            frameHeight
        );
    }
}
