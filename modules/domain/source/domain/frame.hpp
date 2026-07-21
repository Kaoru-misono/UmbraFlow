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
        FrameId m_id;
        SessionId m_sessionId;
        TargetGeneration m_targetGeneration;
        MonotonicInstant m_capturedAt;
        uint32 m_width;
        uint32 m_height;
        std::size_t m_stride;
        PixelFormat m_pixelFormat;
        std::shared_ptr<FrameBuffer const> m_pixels;
        CoordinateTransform m_transform;

        Frame(
            FrameId id,
            SessionId sessionId,
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
            SessionId sessionId,
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
        [[nodiscard]] auto sessionId() const noexcept -> SessionId;
        [[nodiscard]] auto targetGeneration() const noexcept -> TargetGeneration;
        [[nodiscard]] auto capturedAt() const noexcept -> MonotonicInstant;
        [[nodiscard]] auto width() const noexcept -> uint32;
        [[nodiscard]] auto height() const noexcept -> uint32;
        [[nodiscard]] auto stride() const noexcept -> std::size_t;
        [[nodiscard]] auto pixelFormat() const noexcept -> PixelFormat;
        [[nodiscard]] auto pixels() const noexcept -> std::shared_ptr<FrameBuffer const>;
        [[nodiscard]] auto transform() const noexcept -> CoordinateTransform;
    };
}

UF_REFLECT_ENUM(
    uf::PixelFormat,
    uf::PixelFormat::Bgra8,
    uf::PixelFormat::Gray8
);
