#pragma once

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>

namespace uf::controller_detail
{
    // Freshness follows frame arrival time, not pixel changes or consumption time.
    class StallTracker final
    {
        MonotonicInstant::Duration m_timeout;
        MonotonicInstant m_lastArrival;

    public:
        constexpr StallTracker(
            MonotonicInstant::Duration timeout,
            MonotonicInstant startedAt
        ) noexcept
            : m_timeout{timeout}
            , m_lastArrival{startedAt}
        {
        }

        auto onFrameArrived(MonotonicInstant now) noexcept -> void;

        [[nodiscard]]
        constexpr auto lastArrival() const noexcept -> MonotonicInstant
        {
            return m_lastArrival;
        }

        [[nodiscard]]
        constexpr auto timeout() const noexcept -> MonotonicInstant::Duration
        {
            return m_timeout;
        }

        [[nodiscard]] auto check(MonotonicInstant now) const -> Status;
    };
}
