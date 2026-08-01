#pragma once

#include "ids.hpp"
#include "space.hpp"
#include "time.hpp"

#include <core/error/contracts.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/enum-reflection.hpp>
#include <core/types/integer.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace uf
{
    enum class PixelFormat : uint8
    {
        Bgra8,
        Gray8,
    };

    [[nodiscard]]
    constexpr auto bytesPerPixel(PixelFormat pixelFormat) noexcept -> std::size_t
    {
        switch (pixelFormat)
        {
        case PixelFormat::Bgra8: return 4;
        case PixelFormat::Gray8: return 1;
        }

        UF_UNREACHABLE_MSG("Unknown PixelFormat value");
    }

    class FrameBuffer final
    {
        std::vector<std::byte> m_data;

    public:
        explicit FrameBuffer(std::vector<std::byte> data) noexcept;
        FrameBuffer(FrameBuffer const&) = default;
        FrameBuffer(FrameBuffer&&) noexcept = default;

        auto operator=(FrameBuffer const&) -> FrameBuffer& = delete;
        auto operator=(FrameBuffer&&) -> FrameBuffer& = delete;
        auto operator==(FrameBuffer const&) const -> bool = default;

        [[nodiscard]]
        auto bytes() const noexcept UF_LIFETIME_BOUND -> std::span<std::byte const>;

        [[nodiscard]] auto size() const noexcept -> std::size_t;
        [[nodiscard]] auto empty() const noexcept -> bool;
    };

    [[nodiscard]]
    auto validateBufferGeometry(
        uint32 width,
        uint32 height,
        std::size_t stride,
        std::size_t minimumRowBytes,
        std::size_t bufferLength
    ) -> Status;

    class Frame final
    {
        FrameId          m_id;
        CaptureSessionId m_sessionId;
        TargetGeneration m_targetGeneration;
        MonotonicInstant m_capturedAt;

        uint32      m_width;
        uint32      m_height;
        std::size_t m_stride;
        PixelFormat m_pixelFormat;

        std::shared_ptr<FrameBuffer const> m_pixels;
        CoordinateTransform                m_transform;

        Frame(
            FrameId id,
            CaptureSessionId sessionId,
            TargetGeneration targetGeneration,
            MonotonicInstant capturedAt,
            uint32 width,
            uint32 height,
            std::size_t stride,
            PixelFormat pixelFormat,
            std::shared_ptr<FrameBuffer const> p_pixels,
            CoordinateTransform transform
        ) noexcept;

    public:
        [[nodiscard]]
        static auto create(
            FrameId id,
            CaptureSessionId sessionId,
            TargetGeneration targetGeneration,
            MonotonicInstant capturedAt,
            uint32 width,
            uint32 height,
            std::size_t stride,
            PixelFormat pixelFormat,
            std::shared_ptr<FrameBuffer const> p_pixels,
            CoordinateTransform transform
        ) -> Result<Frame>;

        [[nodiscard]] auto id() const noexcept -> FrameId;
        [[nodiscard]] auto sessionId() const noexcept -> CaptureSessionId;
        [[nodiscard]] auto targetGeneration() const noexcept -> TargetGeneration;
        [[nodiscard]] auto capturedAt() const noexcept -> MonotonicInstant;
        [[nodiscard]] auto width() const noexcept -> uint32;
        [[nodiscard]] auto height() const noexcept -> uint32;
        [[nodiscard]] auto stride() const noexcept -> std::size_t;
        [[nodiscard]] auto pixelFormat() const noexcept -> PixelFormat;
        [[nodiscard]] auto pixels() const noexcept -> std::shared_ptr<FrameBuffer const>;
        [[nodiscard]] auto transform() const noexcept -> CoordinateTransform;
    };

    // Which capture one piece of evidence belongs to: the capture session, the
    // target generation within it, and the frame.
    //
    // It is the join key every layer stamps onto its own record, and it is one
    // type rather than three loose fields precisely so a site that leaves a part
    // of it out does not compile -- there is no default constructor. It lives in
    // domain because the frame does: naming a frame is not a fact about any
    // model of the screen, and the layers that name one must not need a model's
    // vocabulary to do it.
    class FrameIdentity final
    {
        CaptureSessionId m_sessionId;
        TargetGeneration m_targetGeneration;
        FrameId          m_frameId;

    public:
        constexpr FrameIdentity(
            CaptureSessionId sessionId,
            TargetGeneration targetGeneration,
            FrameId frameId
        ) noexcept
            : m_sessionId{sessionId}
            , m_targetGeneration{targetGeneration}
            , m_frameId{frameId}
        {
        }

        auto operator<=>(FrameIdentity const&) const = default;

        [[nodiscard]] static auto fromFrame(Frame const& frame) noexcept -> FrameIdentity;

        [[nodiscard]] auto sessionId() const noexcept -> CaptureSessionId;
        [[nodiscard]] auto targetGeneration() const noexcept -> TargetGeneration;
        [[nodiscard]] auto frameId() const noexcept -> FrameId;
    };
}

UF_REFLECT_ENUM(
    uf::PixelFormat,
    uf::PixelFormat::Bgra8,
    uf::PixelFormat::Gray8
);
