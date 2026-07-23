#include "windows-console-control.hpp"

#include <core/error/result.hpp>

#include <domain/error.hpp>

#include <Windows.h>

#include <atomic>
#include <format>
#include <system_error>

namespace uf::m0_demo::platform
{
    namespace
    {
        static_assert(std::atomic_bool::is_always_lock_free);

        [[nodiscard]]
        auto stopFlag() noexcept -> std::atomic_bool&
        {
            static auto s_stop = std::atomic_bool{false};
            return s_stop;
        }

        auto WINAPI handleConsoleControl(DWORD controlType) noexcept -> BOOL
        {
            if (controlType != CTRL_C_EVENT && controlType != CTRL_BREAK_EVENT)
            {
                return FALSE;
            }

            stopFlag().store(true, std::memory_order_seq_cst);
            return TRUE;
        }
    }

    auto installConsoleControlHandler() -> Status
    {
        stopFlag().store(false, std::memory_order_seq_cst);

        // SAFETY: handleConsoleControl has the required WINAPI callback ABI and
        // accesses only a module-owned process-lifetime lock-free atomic.
        if (SetConsoleCtrlHandler(handleConsoleControl, TRUE) == FALSE)
        {
            auto const error = GetLastError();
            return fail(
                AutomationErrorKind::ExternalFailure,
                std::format(
                    "failed to install Ctrl-C handler: Win32 error {}",
                    error
                ),
                systemErrorCode(error)
            );
        }
        return ok();
    }

    auto uninstallConsoleControlHandler() -> Status
    {
        // SAFETY: this removes the exact callback registered by
        // installConsoleControlHandler and does not transfer its address.
        if (SetConsoleCtrlHandler(handleConsoleControl, FALSE) == FALSE)
        {
            auto const error = GetLastError();
            return fail(
                AutomationErrorKind::ExternalFailure,
                std::format(
                    "failed to uninstall Ctrl-C handler: Win32 error {}",
                    error
                ),
                systemErrorCode(error)
            );
        }
        return ok();
    }

    auto stopRequested() noexcept -> bool
    {
        return stopFlag().load(std::memory_order_seq_cst);
    }
}
