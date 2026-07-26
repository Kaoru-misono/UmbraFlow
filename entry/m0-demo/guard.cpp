#include "guard.hpp"

#include "platform/windows-guard.hpp"

#include <core/types/integer.hpp>

#include <optional>
#include <string_view>

namespace uf::m0_demo
{
    auto IntegrityLevel::label() const noexcept -> std::string_view
    {
        if (m_rid >= 0x4000U)
        {
            return "system";
        }
        if (m_rid >= 0x3000U)
        {
            return "high";
        }
        if (m_rid >= 0x2000U)
        {
            return "medium";
        }
        if (m_rid >= 0x1000U)
        {
            return "low";
        }
        return "untrusted";
    }

    auto GuardPolicy::forMode(Mode mode) noexcept -> GuardPolicy
    {
        switch (mode)
        {
        case Mode::Guard: return GuardPolicy{true, true};
        case Mode::Coexist: return GuardPolicy{false, false};
        }

        return GuardPolicy{true, true};
    }

    auto checkGuard(
        GuardPolicy policy,
        intptr targetWindow,
        GuardBaseline baseline,
        GuardBaseline observed
    ) noexcept -> GuardCheck
    {
        return GuardCheck{
            .baselineBackgroundOk = (
                !policy.compareForeground
                || (baseline.foreground != 0 && baseline.foreground != targetWindow)
            ),
            .foregroundOk = (
                !policy.compareForeground
                || baseline.foreground == observed.foreground
            ),
            .cursorOk = !policy.compareCursor || baseline.cursor == observed.cursor,
        };
    }

    auto observeGuard(GuardPolicy policy) -> Result<GuardBaseline>
    {
        return platform::observeGuard(policy);
    }

    auto clientOriginDesktop(WindowHandle windowHandle) -> Result<Point<DesktopSpace>>
    {
        return platform::clientOriginDesktop(windowHandle);
    }

    auto currentProcessIntegrity() -> std::optional<IntegrityLevel>
    {
        return platform::currentProcessIntegrity();
    }

    auto processIntegrity(ProcessId process) -> std::optional<IntegrityLevel>
    {
        return platform::processIntegrity(process);
    }
}
