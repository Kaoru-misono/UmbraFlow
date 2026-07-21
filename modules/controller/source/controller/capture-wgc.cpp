#include "detail/capture-wgc.hpp"

#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <cmath>
#include <format>
#include <string_view>
#include <utility>

namespace uf::controller_detail
{
    namespace
    {
        [[nodiscard]]
        auto captureDimensions(
            controller_detail::CaptureSize size,
            std::string_view context
        ) -> Result<std::pair<uint32, uint32>>
        {
            auto const width = checkedCast<uint32>(size.m_width);
            auto const height = checkedCast<uint32>(size.m_height);
            if (!width || !height)
            {
                return fail(
                    AutomationErrorKind::CaptureUnavailable,
                    std::format(
                        "{} has invalid signed dimensions {}x{}",
                        context,
                        size.m_width,
                        size.m_height
                    )
                );
            }

            if (*width == 0 || *height == 0)
            {
                return fail(
                    AutomationErrorKind::CaptureUnavailable,
                    std::format(
                        "{} must be positive, got {}x{}",
                        context,
                        *width,
                        *height
                    )
                );
            }

            return std::pair{*width, *height};
        }

        [[nodiscard]]
        auto exactClientDimension(
            float value,
            std::string_view context
        ) -> Result<uint32>
        {
            constexpr auto maximumExact = static_cast<float>(uint64{1} << 24U);
            if (
                !std::isfinite(value)
                || value < 0.0F
                || std::trunc(value) != value
                || value > maximumExact
            )
            {
                return fail(
                    AutomationErrorKind::CaptureUnavailable,
                    std::format("{} {} is not a whole pixel count in range", context, value)
                );
            }

            auto const converted = checkedIntegralCast<uint32>(value);
            if (!converted)
            {
                return fail(
                    AutomationErrorKind::CaptureUnavailable,
                    std::format("{} {} is not a whole pixel count in range", context, value)
                );
            }

            return *converted;
        }
    }
}

namespace uf::controller_detail
{
    auto FrameIdCounter::nextId() -> Result<FrameId>
    {
        auto const next = checkedAdd(m_next, uint64{1});
        if (!next)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "frame id counter overflow"
            );
        }

        auto const id = FrameId{m_next};
        m_next = *next;
        return id;
    }

    auto CaptureGeometryState::create(
        CaptureSize initialSize
    ) -> Result<CaptureGeometryState>
    {
        UF_TRY_VALUE(
            dimensions,
            captureDimensions(initialSize, "initial GraphicsCaptureItem size")
        );
        return CaptureGeometryState{dimensions.first, dimensions.second};
    }

    auto CaptureGeometryState::ensureActive() const -> Status
    {
        if (m_invalidated)
        {
            return fail(
                AutomationErrorKind::CaptureUnavailable,
                "capture geometry was invalidated; rebuild the capture session from a freshly resolved target"
            );
        }

        return ok();
    }

    auto CaptureGeometryState::observeContentSize(
        CaptureSize contentSize
    ) -> Result<std::pair<uint32, uint32>>
    {
        UF_TRY(ensureActive());

        auto observedResult = captureDimensions(contentSize, "captured content size");
        if (!observedResult)
        {
            m_invalidated = true;
            return std::unexpected{std::move(observedResult).error()};
        }
        auto const observed = *observedResult;

        if (observed != std::pair{m_expectedWidth, m_expectedHeight})
        {
            m_invalidated = true;
            return fail(
                AutomationErrorKind::CaptureUnavailable,
                std::format(
                    "captured content size {}x{} changed from {}x{}; target must be re-resolved and the capture session rebuilt",
                    observed.first,
                    observed.second,
                    m_expectedWidth,
                    m_expectedHeight
                )
            );
        }

        return observed;
    }

    auto CaptureGeometryState::observeSurfaceSize(
        std::pair<uint32, uint32> confirmedContentSize,
        uint32 surfaceWidth,
        uint32 surfaceHeight
    ) -> Status
    {
        UF_TRY(ensureActive());

        if (std::pair{surfaceWidth, surfaceHeight} != confirmedContentSize)
        {
            m_invalidated = true;
            return fail(
                AutomationErrorKind::CaptureUnavailable,
                std::format(
                    "capture surface size {}x{} does not match confirmed content size {}x{}; rebuild the capture session",
                    surfaceWidth,
                    surfaceHeight,
                    confirmedContentSize.first,
                    confirmedContentSize.second
                )
            );
        }

        return ok();
    }

    auto clientIntegerExtent(
        ClientGeometry const& client
    ) -> Result<std::pair<uint32, uint32>>
    {
        UF_TRY_VALUE(width, exactClientDimension(client.width(), "client width"));
        UF_TRY_VALUE(height, exactClientDimension(client.height(), "client height"));
        return std::pair{width, height};
    }
}
