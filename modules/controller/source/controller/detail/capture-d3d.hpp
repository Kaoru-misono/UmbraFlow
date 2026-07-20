#pragma once

#include <core/error/result.hpp>

#include <cstddef>
#include <cstdint>
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
        std::uint32_t m_offsetX;
        std::uint32_t m_offsetY;
        std::uint32_t m_width;
        std::uint32_t m_height;
        std::uint32_t m_right;
        std::uint32_t m_bottom;

        constexpr ClientCropRect(
            std::uint32_t offsetX,
            std::uint32_t offsetY,
            std::uint32_t width,
            std::uint32_t height,
            std::uint32_t right,
            std::uint32_t bottom
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
            std::pair<std::uint32_t, std::uint32_t> frame,
            std::pair<std::int32_t, std::int32_t> extended,
            std::pair<std::int32_t, std::int32_t> offset,
            std::pair<std::uint32_t, std::uint32_t> client
        ) -> Result<ClientCropRect>;

        [[nodiscard]]
        constexpr auto offsetX() const noexcept -> std::uint32_t
        {
            return m_offsetX;
        }

        [[nodiscard]]
        constexpr auto offsetY() const noexcept -> std::uint32_t
        {
            return m_offsetY;
        }

        [[nodiscard]] constexpr auto width() const noexcept -> std::uint32_t { return m_width; }
        [[nodiscard]] constexpr auto height() const noexcept -> std::uint32_t { return m_height; }
        [[nodiscard]] constexpr auto right() const noexcept -> std::uint32_t { return m_right; }
        [[nodiscard]] constexpr auto bottom() const noexcept -> std::uint32_t { return m_bottom; }

        [[nodiscard]]
        auto ensureWithinSource(
            std::uint32_t sourceWidth,
            std::uint32_t sourceHeight
        ) const -> Status;
    };

    // Removes D3D row-pitch padding and returns tightly strided BGRA8 rows.
    [[nodiscard]]
    auto readbackBgra8(
        std::span<std::byte const> source,
        std::size_t rowPitch,
        std::uint32_t width,
        std::uint32_t height
    ) -> Result<std::vector<std::byte>>;
}
