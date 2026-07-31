#include "detail/capture-stall.hpp"

#include <core/error/contracts.hpp>
#include <domain/error.hpp>

#include <format>
#include <string_view>

namespace uf::controller_detail
{
    namespace
    {
        // Naming the symptom alone cost an operator real time on 2026-07-31: the
        // game window was minimized, which is a one-second fix, and every stalled
        // capture said only that no frame had arrived. Each explanation below
        // therefore states the window state and the action that clears it.
        constexpr auto k_composingExplanation = std::string_view{
            "the target window still exists and is not minimized, so its window "
            "state does not explain the stall; the application itself is not "
            "producing frames"
        };

        constexpr auto k_minimizedExplanation = std::string_view{
            "the target window is minimized, and a minimized window composites no "
            "frames at all, so a stall is the expected result rather than a capture "
            "fault. Restore the window from the taskbar and run again; a target "
            "running elevated has to be restored from an elevated context"
        };

        constexpr auto k_destroyedExplanation = std::string_view{
            "the target window no longer exists, so nothing can composite for it. "
            "Resolve the target again and rebuild the capture session"
        };

        [[nodiscard]]
        constexpr auto stallExplanation(
            TargetWindowState observed
        ) noexcept -> std::string_view
        {
            switch (observed)
            {
            case TargetWindowState::Composing: return k_composingExplanation;
            case TargetWindowState::Minimized: return k_minimizedExplanation;
            case TargetWindowState::Destroyed: return k_destroyedExplanation;
            }
            UF_UNREACHABLE_MSG("Unknown TargetWindowState value");
        }
    }

    auto stalledFrameFailure(
        MonotonicInstant::Duration waited,
        TargetWindowState observed,
        std::source_location location
    ) -> std::unexpected<Error>
    {
        return fail(
            AutomationErrorKind::CaptureStalled,
            std::format(
                "no new frame arrived within {} monotonic clock ticks: {}",
                waited.count(),
                stallExplanation(observed)
            ),
            {},
            location
        );
    }

    auto StallTracker::onFrameArrived(MonotonicInstant now) noexcept -> void
    {
        m_lastArrival = now;
    }

    auto StallTracker::check(
        MonotonicInstant now,
        TargetWindowState observed
    ) const -> Status
    {
        if (now.saturatingDurationSince(m_lastArrival) > m_timeout)
        {
            return stalledFrameFailure(m_timeout, observed);
        }

        return ok();
    }
}
