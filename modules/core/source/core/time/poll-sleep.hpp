#pragma once

#include "core/time/monotonic-time.hpp"

#include <chrono>
#include <stop_token>

namespace uf
{
    // The longest a single poll sleep blocks before it re-checks cancellation
    // and the deadline. It bounds cancellation latency when the poll interval
    // is large, without changing the effective poll cadence.
    inline constexpr auto k_maxPollSleepSlice = MonotonicInstant::Duration{
        std::chrono::milliseconds{100}
    };

    // Sleeps for up to `interval`, in slices no longer than k_maxPollSleepSlice,
    // and stops early once cancellation is requested or the deadline has passed.
    // Sleeping is its only effect: the caller re-checks both conditions and
    // decides the outcome, so this only bounds latency.
    //
    // It lives in core because two modules poll on the same contract -- the
    // engine's page wait and the task layer's `wait` and `settle` primitives --
    // and neither may own a sleep loop the other copies. The deliberate limit is
    // that a stop is observed at a slice boundary rather than immediately: there
    // is nothing to notify, so waking early would cost a condition variable per
    // call site for latency the slice already bounds.
    auto pollSleep(
        MonotonicInstant::Duration interval,
        MonotonicInstant deadline,
        std::stop_token const& cancellation
    ) -> void;
}
