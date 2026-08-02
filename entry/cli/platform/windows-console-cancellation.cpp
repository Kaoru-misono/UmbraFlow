#include "windows-console-cancellation.hpp"

#include <core/error/result.hpp>

#include <domain/error.hpp>

#include <Windows.h>

#include <format>
#include <stop_token>
#include <system_error>

namespace uf::cli::platform
{
    namespace
    {
        [[nodiscard]]
        auto cancellationSource() noexcept -> std::stop_source&
        {
            // Process-lifetime: once a Ctrl-C stops this source it stays stopped,
            // so a second runProduct() in the same process would observe an
            // already-stopped token and refuse to act. The CLI runs exactly one
            // run per process by design (P0), so each invocation starts unstopped.
            static auto s_source = std::stop_source{};
            return s_source;
        }

        auto WINAPI handleConsoleControl(DWORD controlType) noexcept -> BOOL
        {
            if (controlType != CTRL_C_EVENT && controlType != CTRL_BREAK_EVENT)
            {
                return FALSE;
            }

            static_cast<void>(cancellationSource().request_stop());
            return TRUE;
        }
    }

    ConsoleCancellation::~ConsoleCancellation()
    {
        if (m_active)
        {
            // SAFETY: this removes the exact callback registered by install() and
            // does not transfer its address; a failed removal is not actionable.
            static_cast<void>(SetConsoleCtrlHandler(handleConsoleControl, FALSE));
        }
    }

    auto ConsoleCancellation::install() -> Result<ConsoleCancellation>
    {
        // SAFETY: handleConsoleControl has the required WINAPI callback ABI and
        // touches only a module-owned process-lifetime stop_source.
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
        return ConsoleCancellation{};
    }

    auto ConsoleCancellation::stopRequested() noexcept -> bool
    {
        return cancellationSource().stop_requested();
    }

    auto ConsoleCancellation::token() const noexcept -> std::stop_token
    {
        return cancellationSource().get_token();
    }
}
