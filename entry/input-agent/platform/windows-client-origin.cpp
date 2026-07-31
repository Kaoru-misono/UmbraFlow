#include "windows-client-origin.hpp"

#include <domain/error.hpp>

#include <Windows.h>

#include <bit>
#include <format>

namespace uf::input_agent::platform
{
    namespace
    {
        [[nodiscard]]
        auto windowFrom(WindowHandle handle) noexcept -> HWND
        {
            // SAFETY: WindowHandle stores the pointer-sized integer representation copied
            // from an HWND. bit_cast restores those exact bits as the opaque token without
            // dereferencing it.
            return std::bit_cast<HWND>(handle.value());
        }
    }

    auto clientOriginDesktop(WindowHandle windowHandle) -> Result<Point<DesktopSpace>>
    {
        auto const window = windowFrom(windowHandle);
        auto windowRectangle = RECT{};
        // SAFETY: window is an opaque candidate handle and windowRectangle is a
        // live out-parameter. Failure is handled as target unavailability.
        if (GetWindowRect(window, &windowRectangle) == FALSE)
        {
            return fail(
                AutomationErrorKind::TargetUnavailable,
                std::format("GetWindowRect failed with Win32 error {}", GetLastError())
            );
        }

        auto origin = POINT{};
        // SAFETY: origin is initialized to client (0, 0), remains live for the
        // in-place translation, and is consumed only when the call succeeds.
        if (ClientToScreen(window, &origin) == FALSE)
        {
            return fail(
                AutomationErrorKind::TargetUnavailable,
                std::format("ClientToScreen failed with Win32 error {}", GetLastError())
            );
        }

        return Point<DesktopSpace>{
            static_cast<float>(origin.x),
            static_cast<float>(origin.y)
        };
    }
}
