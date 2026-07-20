#include "frame.hpp"

#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>

#include <format>
#include <memory>
#include <utility>

namespace uf
{
    FrameBuffer::FrameBuffer(std::vector<std::byte> data) noexcept
        : m_data{std::move(data)}
    {
    }

    auto FrameBuffer::bytes() const noexcept -> std::span<std::byte const>
    {
        return std::span<std::byte const>{m_data};
    }

    auto FrameBuffer::size() const noexcept -> std::size_t { return m_data.size(); }
    auto FrameBuffer::empty() const noexcept -> bool { return m_data.empty(); }

    auto validateBufferGeometry(
        std::uint32_t width,
        std::uint32_t height,
        std::size_t stride,
        std::size_t minimumRowBytes,
        std::size_t bufferLength
    ) -> Status
    {
        if (width == 0 || height == 0)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format("zero image dimension {}x{}", width, height)
            );
        }

        if (stride < minimumRowBytes)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format(
                    "stride {} smaller than row size {}",
                    stride,
                    minimumRowBytes
                )
            );
        }

        auto const heightSize = checkedCast<std::size_t>(height);
        if (!heightSize)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format("image height {} does not fit buffer geometry", height)
            );
        }

        auto const minimumLength = checkedMultiply(stride, *heightSize);
        if (!minimumLength)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format("image size overflow: stride {} * height {}", stride, height)
            );
        }

        if (bufferLength < *minimumLength)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format(
                    "buffer {} bytes, need at least {}",
                    bufferLength,
                    *minimumLength
                )
            );
        }

        return ok();
    }

    Frame::Frame(
        FrameId id,
        SessionId sessionId,
        TargetGeneration targetGeneration,
        MonotonicInstant capturedAt,
        std::uint32_t width,
        std::uint32_t height,
        std::size_t stride,
        PixelFormat pixelFormat,
        std::shared_ptr<FrameBuffer const> p_pixels,
        CoordinateTransform transform
    ) noexcept
        : m_id{id}
        , m_sessionId{sessionId}
        , m_targetGeneration{targetGeneration}
        , m_capturedAt{capturedAt}
        , m_width{width}
        , m_height{height}
        , m_stride{stride}
        , m_pixelFormat{pixelFormat}
        , m_pixels{std::move(p_pixels)}
        , m_transform{transform}
    {
    }

    auto Frame::create(
        FrameId id,
        SessionId sessionId,
        TargetGeneration targetGeneration,
        MonotonicInstant capturedAt,
        std::uint32_t width,
        std::uint32_t height,
        std::size_t stride,
        PixelFormat pixelFormat,
        std::shared_ptr<FrameBuffer const> p_pixels,
        CoordinateTransform transform
    ) -> Result<Frame>
    {
        if (!p_pixels)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "frame pixel buffer cannot be null"
            );
        }

        auto const widthSize = checkedCast<std::size_t>(width);
        if (!widthSize)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format("image width {} does not fit buffer geometry", width)
            );
        }

        auto const minimumStride = checkedMultiply(*widthSize, bytesPerPixel(pixelFormat));
        if (!minimumStride)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format("minimum stride overflow for image width {}", width)
            );
        }

        UF_TRY(
            validateBufferGeometry(
                width,
                height,
                stride,
                *minimumStride,
                p_pixels->size()
            )
        );

        auto const [transformWidth, transformHeight] = transform.frameSize();
        if (transformWidth != width || transformHeight != height)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format(
                    "transform frame size {}x{} disagrees with frame {}x{}",
                    transformWidth,
                    transformHeight,
                    width,
                    height
                )
            );
        }

        return Frame{
            id,
            sessionId,
            targetGeneration,
            capturedAt,
            width,
            height,
            stride,
            pixelFormat,
            std::move(p_pixels),
            transform
        };
    }

    auto Frame::id() const noexcept -> FrameId { return m_id; }
    auto Frame::sessionId() const noexcept -> SessionId { return m_sessionId; }
    auto Frame::targetGeneration() const noexcept -> TargetGeneration
    {
        return m_targetGeneration;
    }
    auto Frame::capturedAt() const noexcept -> MonotonicInstant { return m_capturedAt; }
    auto Frame::width() const noexcept -> std::uint32_t { return m_width; }
    auto Frame::height() const noexcept -> std::uint32_t { return m_height; }
    auto Frame::stride() const noexcept -> std::size_t { return m_stride; }
    auto Frame::pixelFormat() const noexcept -> PixelFormat { return m_pixelFormat; }
    auto Frame::pixels() const noexcept -> std::shared_ptr<FrameBuffer const> { return m_pixels; }
    auto Frame::transform() const noexcept -> CoordinateTransform { return m_transform; }
}
