#include "windows-controller.hpp"

#include "controller/detail/dpi-classification.hpp"

#include <Windows.h>

#include <cstdint>
#include <optional>

namespace uf::controller_platform
{
    auto setPerMonitorAwareV2() noexcept -> DpiSetObservation
    {
        // SAFETY: Clearing the calling thread's last-error slot has no pointer or lifetime
        // precondition and makes a failed setter's native code unambiguous.
        SetLastError(ERROR_SUCCESS);
        // SAFETY: SetProcessDpiAwarenessContext receives an OS-defined constant and retains
        // no caller-owned pointer or state.
        auto const declared = SetProcessDpiAwarenessContext(
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        );

        auto error = std::optional<std::uint32_t>{};
        if (declared == FALSE)
        {
            // SAFETY: GetLastError reads calling-thread state immediately after the failed
            // setter and does not access caller-owned memory.
            error = controller_detail::win32Code(GetLastError());
        }

        // SAFETY: GetThreadDpiAwarenessContext returns an OS-owned opaque context token and
        // retains no caller-owned state.
        auto const current = GetThreadDpiAwarenessContext();
        // SAFETY: AreDpiAwarenessContextsEqual only compares two OS-owned opaque tokens and
        // retains no caller-owned state.
        auto const isPerMonitorAwareV2 = AreDpiAwarenessContextsEqual(
            current,
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        ) != FALSE;

        return DpiSetObservation{
            .m_win32Error = error,
            .m_isPerMonitorAwareV2 = isPerMonitorAwareV2,
        };
    }
}
