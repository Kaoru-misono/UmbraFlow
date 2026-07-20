#include "windows-console-control.hpp"

#include <domain/error.hpp>

#include <Windows.h>

#include <atomic>
#include <format>

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

namespace uf::m0_demo::platform
{
    auto installConsoleControlHandler() -> Status
    {
        stopFlag().store(false, std::memory_order_seq_cst);

        // SAFETY: handleConsoleControl has the required WINAPI callback ABI and
        // accesses only a module-owned process-lifetime lock-free atomic.
        if (!SetConsoleCtrlHandler(handleConsoleControl, TRUE))
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format(
                    "failed to install Ctrl-C handler: Win32 error {}",
                    GetLastError()
                )
            );
        }
        return ok();
    }

    auto stopRequested() noexcept -> bool
    {
        return stopFlag().load(std::memory_order_seq_cst);
    }
}
