#include "dpi.hpp"

#include "detail/dpi-classification.hpp"
#include "platform/windows-controller.hpp"

#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <format>
#include <optional>

namespace uf
{
    auto ensurePerMonitorAwareV2() -> Result<DpiDeclaration>
    {
        auto const observation = controller_platform::setPerMonitorAwareV2();
        return controller_detail::classifyDpiResult(
            observation.m_win32Error,
            observation.m_isPerMonitorAwareV2
        );
    }
}

namespace uf::controller_detail
{
    auto classifyDpiResult(
        std::optional<uint32> win32Error,
        bool isPerMonitorAwareV2
    ) -> Result<DpiDeclaration>
    {
        if (!win32Error && isPerMonitorAwareV2)
        {
            return DpiDeclaration::Declared;
        }
        if (
            win32Error == k_accessDeniedError
            && isPerMonitorAwareV2
        )
        {
            return DpiDeclaration::AlreadyDeclared;
        }
        if (
            (!win32Error || win32Error == k_accessDeniedError)
            && !isPerMonitorAwareV2
        )
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "process DPI awareness is not PER_MONITOR_AWARE_V2"
            );
        }

        return fail(
            AutomationErrorKind::InternalInvariant,
            std::format(
                "SetProcessDpiAwarenessContext failed with win32 error {}",
                *win32Error
            )
        );
    }
}
