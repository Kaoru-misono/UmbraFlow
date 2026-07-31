#include "target-setup.hpp"

#include "platform/windows-client-origin.hpp"

#include <domain/error.hpp>

#include <format>
#include <utility>

namespace uf::input_agent
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

    auto requireUnchangedTarget(RevalidateOutcome outcome) -> Status
    {
        switch (outcome)
        {
        case RevalidateOutcome::Unchanged:
            return ok();
        case RevalidateOutcome::GenerationBumped:
            return fail(
                AutomationErrorKind::StaleObservation,
                "target generation changed; rebuild the capture/input session"
            );
        case RevalidateOutcome::InstanceUnconfirmed:
            return fail(
                AutomationErrorKind::StaleObservation,
                "target process instance is unconfirmed; re-resolve before continuing"
            );
        case RevalidateOutcome::Lost:
        {
            auto lost = errorOnLost(outcome);
            if (!lost)
            {
                return std::unexpected{std::move(lost).error()};
            }
            return ok();
        }
        }

        return fail(
            AutomationErrorKind::InternalInvariant,
            "unknown target revalidation outcome"
        );
    }
}
