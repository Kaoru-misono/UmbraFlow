#pragma once

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace uf::controller_detail
{
    // Client-area sub-rectangle within a full-window WGC frame. The extended extent must
    // come from DWMWA_EXTENDED_FRAME_BOUNDS, never GetWindowRect, because the latter can
    // include an invisible resize border that WGC excludes.
    class ClientCropRect final
    {
        uint32 m_offsetX;
        uint32 m_offsetY;
        uint32 m_width;
        uint32 m_height;
        uint32 m_right;
        uint32 m_bottom;

        constexpr ClientCropRect(
            uint32 offsetX,
            uint32 offsetY,
            uint32 width,
            uint32 height,
            uint32 right,
            uint32 bottom
        ) noexcept
            : m_offsetX{offsetX}
            , m_offsetY{offsetY}
            , m_width{width}
            , m_height{height}
            , m_right{right}
            , m_bottom{bottom}
        {
        }

    public:
        auto operator==(ClientCropRect const&) const -> bool = default;

        [[nodiscard]]
        static auto create(
            std::pair<uint32, uint32> frame,
            std::pair<int32, int32> extended,
            std::pair<int32, int32> offset,
            std::pair<uint32, uint32> client
        ) -> Result<ClientCropRect>;

        [[nodiscard]]
        constexpr auto offsetX() const noexcept -> uint32
        {
            return m_offsetX;
        }

        [[nodiscard]]
        constexpr auto offsetY() const noexcept -> uint32
        {
            return m_offsetY;
        }

        [[nodiscard]] constexpr auto width() const noexcept -> uint32 { return m_width; }
        [[nodiscard]] constexpr auto height() const noexcept -> uint32 { return m_height; }
        [[nodiscard]] constexpr auto right() const noexcept -> uint32 { return m_right; }
        [[nodiscard]] constexpr auto bottom() const noexcept -> uint32 { return m_bottom; }

        [[nodiscard]]
        auto ensureWithinSource(
            uint32 sourceWidth,
            uint32 sourceHeight
        ) const -> Status;
    };

    // Removes D3D row-pitch padding and returns tightly strided BGRA8 rows.
    [[nodiscard]]
    auto readbackBgra8(
        std::span<std::byte const> source,
        std::size_t rowPitch,
        uint32 width,
        uint32 height
    ) -> Result<std::vector<std::byte>>;
}
