#include "detail/capture-stall.hpp"

#include <domain/error.hpp>

#include <format>

namespace uf::controller_detail
{
    auto StallTracker::onFrameArrived(MonotonicInstant now) noexcept -> void
    {
        m_lastArrival = now;
    }

    auto StallTracker::check(MonotonicInstant now) const -> Status
    {
        if (now.saturatingDurationSince(m_lastArrival) > m_timeout)
        {
            return fail(
                AutomationErrorKind::CaptureStalled,
                std::format(
                    "no new frame arrived within {} monotonic clock ticks",
                    m_timeout.count()
                )
            );
        }

        return ok();
    }
}
