#pragma once

#include "discovery.hpp"

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <chrono>
#include <memory>

namespace uf
{
    inline constexpr auto g_defaultCaptureStallTimeout = MonotonicInstant::Duration{
        std::chrono::seconds{1}
    };

    // Capture tuning. Borderless capture is a fail-closed request until a caller-owned
    // GraphicsCaptureAccess::RequestAccessAsync(Borderless) grant path is added.
    class WgcCaptureOptions final
    {
        MonotonicInstant::Duration m_captureStallTimeout;
        bool                       m_requireBorderless;

        constexpr WgcCaptureOptions(
            MonotonicInstant::Duration captureStallTimeout,
            bool requireBorderless
        ) noexcept
            : m_captureStallTimeout{captureStallTimeout}
            , m_requireBorderless{requireBorderless}
        {
        }

    public:
        constexpr WgcCaptureOptions() noexcept
            : m_captureStallTimeout{g_defaultCaptureStallTimeout}
            , m_requireBorderless{false}
        {
        }

        [[nodiscard]]
        static auto create(
            MonotonicInstant::Duration captureStallTimeout,
            bool requireBorderless
        ) -> Result<WgcCaptureOptions>;

        [[nodiscard]]
        constexpr auto captureStallTimeout() const noexcept -> MonotonicInstant::Duration
        {
            return m_captureStallTimeout;
        }

        [[nodiscard]]
        constexpr auto requireBorderless() const noexcept -> bool
        {
            return m_requireBorderless;
        }
    };

    // Physical client-area geometry on the desktop, supplied by the target owner.
    class ClientGeometry final
    {
        Point<DesktopSpace> m_origin;
        float               m_width;
        float               m_height;

        constexpr ClientGeometry(
            Point<DesktopSpace> origin,
            float width,
            float height
        ) noexcept
            : m_origin{origin}
            , m_width{width}
            , m_height{height}
        {
        }

    public:
        [[nodiscard]]
        static auto create(
            Point<DesktopSpace> origin,
            float width,
            float height
        ) -> Result<ClientGeometry>;

        [[nodiscard]]
        constexpr auto origin() const noexcept -> Point<DesktopSpace>
        {
            return m_origin;
        }

        [[nodiscard]] constexpr auto width() const noexcept -> float { return m_width; }
        [[nodiscard]] constexpr auto height() const noexcept -> float { return m_height; }

        [[nodiscard]]
        auto transformFor(
            uint32 frameWidth,
            uint32 frameHeight
        ) const -> Result<CoordinateTransform>;
    };

    // OS-build-dependent cursor and border state recorded when a session is created.
    struct CaptureHygiene final
    {
        uint32 m_osBuild{};
        bool   m_cursorCaptureDisabled{};
        bool   m_borderlessSupported{};
        bool   m_borderRequired{};

        auto operator==(CaptureHygiene const&) const -> bool = default;
    };

    // A free-threaded Windows Graphics Capture session for one target window. A changed
    // ContentSize latches the session invalid and capture() fails until the owner creates
    // a new session from freshly resolved target geometry.
    class WgcCaptureSession final
    {
        class Impl;

        std::unique_ptr<Impl> m_impl;

        explicit WgcCaptureSession(std::unique_ptr<Impl> p_impl) noexcept;

    public:
        WgcCaptureSession(WgcCaptureSession const&) = delete;
        auto operator=(WgcCaptureSession const&) -> WgcCaptureSession& = delete;
        WgcCaptureSession(WgcCaptureSession&&) noexcept;
        auto operator=(WgcCaptureSession&&) -> WgcCaptureSession& = delete;
        ~WgcCaptureSession();

        [[nodiscard]]
        static auto create(
            WindowHandle windowHandle,
            SessionId sessionId,
            TargetGeneration targetGeneration,
            ClientGeometry client,
            WgcCaptureOptions options = {}
        ) -> Result<WgcCaptureSession>;

        [[nodiscard]] auto capture() -> Result<Frame>;
        [[nodiscard]] auto validateTargetInstance() -> Status;
        [[nodiscard]] auto close() -> Status;

        [[nodiscard]] auto hygiene() const noexcept -> CaptureHygiene;
        [[nodiscard]] auto sessionId() const noexcept -> SessionId;
        [[nodiscard]] auto targetGeneration() const noexcept -> TargetGeneration;
    };
}
