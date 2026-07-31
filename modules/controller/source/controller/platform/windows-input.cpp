#include "windows-controller.hpp"

#include "controller/detail/audit-log-access.hpp"
#include "controller/detail/input-message.hpp"

#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <Windows.h>

#include <bit>
#include <format>

namespace uf::controller_platform
{
    namespace
    {
        [[nodiscard]]
        auto toNativeHandle(WindowHandle handle) noexcept -> HWND
        {
            // SAFETY: WindowHandle stores the pointer-sized integer representation copied
            // from an HWND. bit_cast restores those exact bits as the opaque token without
            // dereferencing it.
            return std::bit_cast<HWND>(handle.value());
        }
    }
}

namespace uf::controller_platform
{
    auto scanCodeFor(uint16 virtualKey) noexcept -> uint8
    {
        // SAFETY: MapVirtualKeyW performs a keyboard-layout lookup for the numeric
        // virtual key and retains no caller-owned pointer or state.
        auto const scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
        return static_cast<uint8>(scanCode & 0xFFU);
    }

    auto clientOriginOnScreen(
        WindowHandle handle
    ) -> Result<controller_detail::ClientOrigin>
    {
        auto const nativeHandle = toNativeHandle(handle);
        auto origin = POINT{.x = 0, .y = 0};
        // SAFETY: ClientToScreen reads the opaque HWND's own geometry and writes only
        // the live stack-local POINT whose address it is given, which is that API's own
        // in-out contract. It retains neither the handle nor the pointer.
        if (ClientToScreen(nativeHandle, &origin) == FALSE)
        {
            // SAFETY: GetLastError reads calling-thread state immediately after the failed
            // ClientToScreen call and does not access caller-owned memory.
            auto const error = GetLastError();
            return fail(
                AutomationErrorKind::ControllerDisconnected,
                std::format(
                    "ClientToScreen failed for window {:#x}: win32 error {}",
                    static_cast<uintptr>(handle.value()),
                    error
                )
            );
        }
        return controller_detail::ClientOrigin{
            .x = static_cast<int32>(origin.x),
            .y = static_cast<int32>(origin.y),
        };
    }

    auto postInputMessage(
        WindowHandle windowHandle,
        controller_detail::PostSpec spec,
        AuditLog& audit
    ) -> Status
    {
        auto const nativeHandle = toNativeHandle(windowHandle);
        // SAFETY: A resolved single-window delivery target is never null or
        // HWND_BROADCAST. Rejecting both preserves valid target behavior and closes
        // paths that could escape target-only delivery through a thread or broadcast.
        if (nativeHandle == nullptr || nativeHandle == HWND_BROADCAST)
        {
            return fail(
                AutomationErrorKind::ControllerDisconnected,
                std::format(
                    "refusing to post input message {:#06x} to non-window target {:#x}",
                    spec.message,
                    static_cast<uintptr>(windowHandle.value())
                )
            );
        }

        controller_detail::AuditLogAccess::record(
            audit,
            windowHandle,
            spec.message,
            spec.wParam,
            spec.lParam
        );

        // SAFETY: PostMessageW only enqueues the plain integer message values for the
        // opaque HWND. It dereferences no caller memory and retains no caller-owned state.
        auto const posted = PostMessageW(
            nativeHandle,
            spec.message,
            spec.wParam,
            spec.lParam
        );
        if (posted != FALSE)
        {
            return ok();
        }

        // SAFETY: GetLastError reads calling-thread state immediately after the failed
        // PostMessageW call and does not access caller-owned memory.
        auto const error = GetLastError();
        return fail(
            AutomationErrorKind::ControllerDisconnected,
            std::format(
                "PostMessageW failed for message {:#06x} to window {:#x}: win32 error {}",
                spec.message,
                static_cast<uintptr>(windowHandle.value()),
                error
            )
        );
    }
}
