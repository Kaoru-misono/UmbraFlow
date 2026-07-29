#include "core/time/poll-sleep.hpp"

#include "core/time/monotonic-time.hpp"

#include <algorithm>
#include <stop_token>
#include <thread>

namespace uf
{
    auto pollSleep(
        MonotonicInstant::Duration interval,
        MonotonicInstant deadline,
        std::stop_token const& cancellation
    ) -> void
    {
        auto remaining = interval;
        while (remaining > MonotonicInstant::Duration::zero())
        {
            if (
                cancellation.stop_requested()
                || MonotonicInstant::now() >= deadline
            )
            {
                return;
            }

            auto const step = std::min(remaining, k_maxPollSleepSlice);
            std::this_thread::sleep_for(step);
            remaining -= step;
        }
    }
}
