#pragma once

#include <controller/capture.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <engine/ports.hpp>

#include <utility>

namespace uf::cli::platform
{
    // Adapts a WgcCaptureSession to the engine IFrameSource port. capture()
    // forwards the session capture and validateTargetInstance() forwards the
    // session's bound-target revalidation, so the engine stays platform-agnostic.
    class WgcFrameSource final : public engine::IFrameSource
    {
        WgcCaptureSession m_session;

    public:
        explicit WgcFrameSource(WgcCaptureSession session) noexcept
            : m_session{std::move(session)}
        {
        }

        // Honours the budget by turning the caller's absolute deadline into the
        // remaining wall time the compositor wait may consume, and by handing the
        // stop token to that wait rather than only testing it here. An expired
        // deadline is a Timeout before any frame work.
        [[nodiscard]]
        auto capture(CaptureBudget const& budget) -> Result<Frame> override
        {
            auto const remaining = budget.deadline.saturatingDurationSince(
                MonotonicInstant::now()
            );
            if (remaining <= MonotonicInstant::Duration::zero())
            {
                return fail(
                    AutomationErrorKind::Timeout,
                    "capture deadline expired before a frame was requested"
                );
            }
            return m_session.capture(remaining, budget.cancellation);
        }

        [[nodiscard]] auto validateTargetInstance() -> Status override
        {
            return m_session.validateTargetInstance();
        }
    };
}
