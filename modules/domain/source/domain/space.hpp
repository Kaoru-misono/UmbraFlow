#pragma once

#include "error.hpp"

#include <core/types/integer.hpp>

#include <concepts>
#include <cstddef>
#include <utility>

namespace uf
{
    struct DesktopSpace final
    {
    };

    struct ClientSpace final
    {
    };

    struct FrameSpace final
    {
    };

    struct NormalizedSpace final
    {
    };

    template <typename Space>
    concept CoordinateSpace = (
        std::same_as<Space, DesktopSpace>
        || std::same_as<Space, ClientSpace>
        || std::same_as<Space, FrameSpace>
        || std::same_as<Space, NormalizedSpace>
    );

    template <CoordinateSpace Space>
    class Point final
    {
        float m_x;
        float m_y;

    public:
        constexpr Point(float x, float y) noexcept
            : m_x{x}
            , m_y{y}
        {
        }

        auto operator==(Point const&) const -> bool = default;

        [[nodiscard]] constexpr auto x() const noexcept -> float { return m_x; }
        [[nodiscard]] constexpr auto y() const noexcept -> float { return m_y; }
    };

    template <CoordinateSpace Space>
    class Rect final
    {
        float m_x;
        float m_y;
        float m_width;
        float m_height;

    public:
        constexpr Rect(float x, float y, float width, float height) noexcept
            : m_x{x}
            , m_y{y}
            , m_width{width}
            , m_height{height}
        {
        }

        auto operator==(Rect const&) const -> bool = default;

        [[nodiscard]] constexpr auto x() const noexcept -> float { return m_x; }
        [[nodiscard]] constexpr auto y() const noexcept -> float { return m_y; }
        [[nodiscard]] constexpr auto width() const noexcept -> float { return m_width; }
        [[nodiscard]] constexpr auto height() const noexcept -> float { return m_height; }

        [[nodiscard]]
        constexpr auto contains(Point<Space> point) const noexcept -> bool
        {
            return (
                point.x() >= m_x
                && point.x() < m_x + m_width
                && point.y() >= m_y
                && point.y() < m_y + m_height
            );
        }

        [[nodiscard]]
        constexpr auto center() const noexcept -> Point<Space>
        {
            return Point<Space>{m_x + m_width / 2.0F, m_y + m_height / 2.0F};
        }

        [[nodiscard]]
        constexpr auto isEmpty() const noexcept -> bool
        {
            return m_width <= 0.0F || m_height <= 0.0F;
        }
    };

    // Every integer in [0, 2^24] inclusive is exactly representable in a
    // 32-bit float; 2^24 + 1 is the first integer that is not. Frame dimensions
    // and pixel edges must stay within this inclusive bound for the
    // integer-to-float geometry conversions to be lossless.
    inline constexpr auto g_maxExactFrameDimension = uint32{1} << 24;

    class PixelPoint final
    {
        uint32 m_x;
        uint32 m_y;

    public:
        constexpr PixelPoint(uint32 x, uint32 y) noexcept
            : m_x{x}
            , m_y{y}
        {
        }

        auto operator==(PixelPoint const&) const -> bool = default;

        [[nodiscard]] constexpr auto x() const noexcept -> uint32 { return m_x; }
        [[nodiscard]] constexpr auto y() const noexcept -> uint32 { return m_y; }
    };

    class PixelRect final
    {
        uint32 m_x;
        uint32 m_y;
        uint32 m_width;
        uint32 m_height;
        uint32 m_right;
        uint32 m_bottom;

        constexpr PixelRect(
            uint32 x,
            uint32 y,
            uint32 width,
            uint32 height,
            uint32 right,
            uint32 bottom
        ) noexcept
            : m_x{x}
            , m_y{y}
            , m_width{width}
            , m_height{height}
            , m_right{right}
            , m_bottom{bottom}
        {
        }

    public:
        auto operator==(PixelRect const&) const -> bool = default;

        [[nodiscard]]
        static auto create(
            uint32 x,
            uint32 y,
            uint32 width,
            uint32 height
        ) -> Result<PixelRect>;

        [[nodiscard]] constexpr auto x() const noexcept -> uint32 { return m_x; }
        [[nodiscard]] constexpr auto y() const noexcept -> uint32 { return m_y; }
        [[nodiscard]] constexpr auto width() const noexcept -> uint32 { return m_width; }
        [[nodiscard]] constexpr auto height() const noexcept -> uint32 { return m_height; }
        [[nodiscard]] constexpr auto right() const noexcept -> uint32 { return m_right; }
        [[nodiscard]] constexpr auto bottom() const noexcept -> uint32 { return m_bottom; }

        [[nodiscard]]
        auto ensureWithinExtent(uint32 width, uint32 height) const -> Status;
    };

    // Exact inverse of the lossy frameRectToPixelRect direction. Fails when an
    // edge exceeds g_maxExactFrameDimension, beyond which float cannot represent
    // the integer coordinate exactly and the conversion would silently round.
    [[nodiscard]]
    auto pixelRectToFrameRect(PixelRect const& rect) -> Result<Rect<FrameSpace>>;

    [[nodiscard]]
    auto pixelPointToFramePoint(PixelPoint point) -> Result<Point<FrameSpace>>;

    class CoordinateTransform final
    {
        static constexpr auto s_frameBoundsEpsilon = 1e-3F;
        static constexpr auto s_maxExactFrameDimension = g_maxExactFrameDimension;

        float  m_clientOriginX;
        float  m_clientOriginY;
        float  m_clientWidth;
        float  m_clientHeight;
        uint32 m_frameWidth;
        uint32 m_frameHeight;

        constexpr CoordinateTransform(
            Point<DesktopSpace> clientOrigin,
            float clientWidth,
            float clientHeight,
            uint32 frameWidth,
            uint32 frameHeight
        ) noexcept
            : m_clientOriginX{clientOrigin.x()}
            , m_clientOriginY{clientOrigin.y()}
            , m_clientWidth{clientWidth}
            , m_clientHeight{clientHeight}
            , m_frameWidth{frameWidth}
            , m_frameHeight{frameHeight}
        {
        }

    public:
        auto operator==(CoordinateTransform const&) const -> bool = default;

    private:
        [[nodiscard]]
        auto frameSizeFloat() const noexcept -> std::pair<float, float>;

    public:
        [[nodiscard]]
        static auto create(
            Point<DesktopSpace> clientOrigin,
            float clientWidth,
            float clientHeight,
            uint32 frameWidth,
            uint32 frameHeight
        ) -> Result<CoordinateTransform>;

        [[nodiscard]]
        constexpr auto frameSize() const noexcept -> std::pair<uint32, uint32>
        {
            return {m_frameWidth, m_frameHeight};
        }

        [[nodiscard]]
        auto desktopToClient(Point<DesktopSpace> point) const noexcept -> Point<ClientSpace>;

        [[nodiscard]]
        auto clientToDesktop(Point<ClientSpace> point) const noexcept -> Point<DesktopSpace>;

        [[nodiscard]]
        auto clientToFrame(Point<ClientSpace> point) const noexcept -> Point<FrameSpace>;

        [[nodiscard]]
        auto frameToClient(Point<FrameSpace> point) const noexcept -> Point<ClientSpace>;

        [[nodiscard]]
        auto frameToNormalized(Point<FrameSpace> point) const noexcept -> Point<NormalizedSpace>;

        [[nodiscard]]
        auto normalizedToFrame(Point<NormalizedSpace> point) const noexcept -> Point<FrameSpace>;

        [[nodiscard]]
        auto desktopToFrame(Point<DesktopSpace> point) const noexcept -> Point<FrameSpace>;

        [[nodiscard]]
        auto frameToDesktop(Point<FrameSpace> point) const noexcept -> Point<DesktopSpace>;

        [[nodiscard]]
        auto clientRectToFrame(Rect<ClientSpace> rect) const noexcept -> Rect<FrameSpace>;

        [[nodiscard]]
        auto frameRectToClient(Rect<FrameSpace> rect) const noexcept -> Rect<ClientSpace>;

        [[nodiscard]]
        auto ensureFrameRectInBounds(Rect<FrameSpace> rect) const -> Status;

        [[nodiscard]]
        auto frameRectToPixelRect(Rect<FrameSpace> rect) const -> Result<PixelRect>;

        [[nodiscard]]
        auto ensureFramePointInBounds(Point<FrameSpace> point) const -> Status;
    };

    struct PixelRectHash final
    {
        [[nodiscard]]
        auto operator()(PixelRect const& rect) const noexcept -> std::size_t;
    };
}
