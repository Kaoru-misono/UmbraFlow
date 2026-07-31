#pragma once

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <source_location>

namespace uf::controller_detail
{
    // What the platform could observe about the target window at the moment no
    // frame had arrived. A minimized or destroyed window composites nothing, so
    // a stall is that state's expected consequence rather than a capture fault,
    // and an operator can clear it in seconds once told. Composing states only
    // that neither cause was observable.
    enum class TargetWindowState : uint8
    {
        Composing,
        Minimized,
        Destroyed,
    };

    // Every CaptureStalled an operator can see is composed here, so that naming
    // the window state is not something a new stall site can forget to do. The
    // location defaults at the call site, so the reported origin stays the fuse
    // that blew rather than this helper.
    [[nodiscard]]
    auto stalledFrameFailure(
        MonotonicInstant::Duration waited,
        TargetWindowState observed,
        std::source_location location = std::source_location::current()
    ) -> std::unexpected<Error>;

    // Freshness follows frame arrival time, not pixel changes or consumption time.
    class StallTracker final
    {
        MonotonicInstant::Duration m_timeout;
        MonotonicInstant           m_lastArrival;

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

        // The observation is a required argument rather than something the
        // tracker could look up: the tracker is portable and owns no window, and
        // making the caller supply it is what stops a stall from being reported
        // without the one fact that explains it.
        [[nodiscard]]
        auto check(
            MonotonicInstant now,
            TargetWindowState observed
        ) const -> Status;
    };
}
