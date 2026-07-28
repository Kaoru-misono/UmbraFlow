#include "detail/capture-readback.hpp"

#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>
#include <domain/error.hpp>
#include <domain/frame.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <span>
#include <utility>
#include <vector>

namespace uf::controller_detail
{
    auto ClientCropRect::create(
        std::pair<uint32, uint32> frame,
        std::pair<int32, int32> extended,
        std::pair<int32, int32> offset,
        std::pair<uint32, uint32> client
    ) -> Result<ClientCropRect>
    {
        auto const [frameWidth, frameHeight] = frame;
        auto const [extendedWidth, extendedHeight] = extended;
        auto const [offsetXSigned, offsetYSigned] = offset;
        auto const [clientWidth, clientHeight] = client;

        if (
            static_cast<int64>(extendedWidth) != static_cast<int64>(frameWidth)
            || static_cast<int64>(extendedHeight)
                != static_cast<int64>(frameHeight)
        )
        {
            return fail(
                AutomationErrorKind::CaptureUnavailable,
                std::format(
                    "DWM extended frame bounds {}x{} do not match WGC frame {}x{}",
                    extendedWidth,
                    extendedHeight,
                    frameWidth,
                    frameHeight
                )
            );
        }

        auto const offsetX = checkedCast<uint32>(offsetXSigned);
        if (!offsetX)
        {
            return fail(
                AutomationErrorKind::CaptureUnavailable,
                std::format(
                    "client x offset {} within the frame is negative",
                    offsetXSigned
                )
            );
        }

        auto const offsetY = checkedCast<uint32>(offsetYSigned);
        if (!offsetY)
        {
            return fail(
                AutomationErrorKind::CaptureUnavailable,
                std::format(
                    "client y offset {} within the frame is negative",
                    offsetYSigned
                )
            );
        }

        if (clientWidth == 0 || clientHeight == 0)
        {
            return fail(
                AutomationErrorKind::CaptureUnavailable,
                std::format(
                    "client crop size must be positive, got {}x{}",
                    clientWidth,
                    clientHeight
                )
            );
        }

        auto const right = checkedAdd(*offsetX, clientWidth);
        if (!right)
        {
            return fail(
                AutomationErrorKind::CaptureUnavailable,
                std::format(
                    "client crop x extent overflow: {} + {}",
                    *offsetX,
                    clientWidth
                )
            );
        }

        auto const bottom = checkedAdd(*offsetY, clientHeight);
        if (!bottom)
        {
            return fail(
                AutomationErrorKind::CaptureUnavailable,
                std::format(
                    "client crop y extent overflow: {} + {}",
                    *offsetY,
                    clientHeight
                )
            );
        }

        if (*right > frameWidth || *bottom > frameHeight)
        {
            return fail(
                AutomationErrorKind::CaptureUnavailable,
                std::format(
                    "client crop ({}, {}, {}, {}) exceeds frame {}x{}",
                    *offsetX,
                    *offsetY,
                    clientWidth,
                    clientHeight,
                    frameWidth,
                    frameHeight
                )
            );
        }

        return ClientCropRect{
            *offsetX,
            *offsetY,
            clientWidth,
            clientHeight,
            *right,
            *bottom
        };
    }

    auto ClientCropRect::ensureWithinSource(
        uint32 sourceWidth,
        uint32 sourceHeight
    ) const -> Status
    {
        if (m_right > sourceWidth || m_bottom > sourceHeight)
        {
            return fail(
                AutomationErrorKind::CaptureUnavailable,
                std::format(
                    "client crop far edge ({}, {}) exceeds source texture {}x{}",
                    m_right,
                    m_bottom,
                    sourceWidth,
                    sourceHeight
                )
            );
        }

        return ok();
    }

    auto readbackBgra8(
        std::span<std::byte const> source,
        std::size_t rowPitch,
        uint32 width,
        uint32 height
    ) -> Result<std::vector<std::byte>>
    {
        if (width == 0 || height == 0)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format("zero readback dimension {}x{}", width, height)
            );
        }

        auto const widthSize = checkedCast<std::size_t>(width);
        auto const heightSize = checkedCast<std::size_t>(height);
        if (!widthSize || !heightSize)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format("readback dimensions {}x{} do not fit size_t", width, height)
            );
        }

        auto const rowBytes = checkedMultiply(*widthSize, bytesPerPixel(PixelFormat::Bgra8));
        if (!rowBytes)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format("readback row size overflow for width {}", *widthSize)
            );
        }

        if (rowPitch < *rowBytes)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format(
                    "row pitch {} smaller than packed row size {}",
                    rowPitch,
                    *rowBytes
                )
            );
        }

        auto const lastRowStart = checkedMultiply(rowPitch, *heightSize - std::size_t{1});
        if (!lastRowStart)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "readback source extent overflow"
            );
        }

        auto const required = checkedAdd(*lastRowStart, *rowBytes);
        if (!required)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "readback source extent overflow"
            );
        }

        if (source.size() < *required)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format(
                    "readback source {} bytes, need at least {}",
                    source.size(),
                    *required
                )
            );
        }

        auto const packedLength = checkedMultiply(*rowBytes, *heightSize);
        if (!packedLength)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "packed buffer size overflow"
            );
        }

        auto packed     = std::vector<std::byte>(*packedLength);
        auto packedView = std::span<std::byte>{packed};
        for (auto row = std::size_t{0}; row < *heightSize; ++row)
        {
            auto const sourceStart = row * rowPitch;
            auto const destinationStart = row * *rowBytes;
            auto const sourceRow = source.subspan(sourceStart, *rowBytes);
            auto const destinationRow = packedView.subspan(destinationStart, *rowBytes);
            std::ranges::copy(sourceRow, destinationRow.begin());
        }

        return packed;
    }
}
