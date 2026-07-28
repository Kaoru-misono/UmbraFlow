#include "target-setup.hpp"

#include "platform/windows-guard.hpp"

#include <domain/error.hpp>

#include <format>

namespace uf::m0_demo
{
    auto ensureClientAreaUsable(ClientSize client) -> Status
    {
        if (client.width() == 0U || client.height() == 0U)
        {
            return fail(
                AutomationErrorKind::TargetUnavailable,
                std::format(
                    "target client area is empty; minimized? ({}x{})",
                    client.width(),
                    client.height()
                )
            );
        }
        return ok();
    }

    auto buildSelector(SelectorArgs const& selector) -> TargetSelector
    {
        auto built = TargetSelector{};
        if (selector.process)
        {
            built = built.withProcess(ProcessId{*selector.process});
        }
        if (selector.windowHandle)
        {
            built = built.withWindowHandle(WindowHandle{*selector.windowHandle});
        }
        if (selector.windowClass)
        {
            built = built.withWindowClass(*selector.windowClass);
        }
        if (selector.title)
        {
            built = built.withTitle(*selector.title);
        }
        return built;
    }

    auto createCaptureSession(
        ResolvedTarget const& target,
        CaptureSessionId sessionId,
        WgcCaptureOptions options
    ) -> Result<WgcCaptureSession>
    {
        auto const client = target.clientSize();
        UF_TRY(ensureClientAreaUsable(client));
        UF_TRY_VALUE(
            origin,
            platform::clientOriginDesktop(target.windowHandle())
        );
        UF_TRY_VALUE(
            geometry,
            ClientGeometry::create(
                origin,
                static_cast<float>(client.width()),
                static_cast<float>(client.height())
            )
        );
        return WgcCaptureSession::create(
            target.windowHandle(),
            sessionId,
            target.currentGeneration(),
            geometry,
            options
        );
    }
}
