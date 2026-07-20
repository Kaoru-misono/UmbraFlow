#include "time.hpp"

#include <core/numeric/checked-cast.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <ratio>
#include <string>

namespace uf
{
    auto checkedAddMonotonic(
        MonotonicInstant instant,
        MonotonicInstant::Duration duration
    ) -> Result<MonotonicInstant>
    {
        if (duration < MonotonicInstant::Duration::zero())
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "monotonic duration cannot be negative: " + std::to_string(duration.count())
            );
        }

        auto const deadline = instant.checkedAdd(duration);
        if (!deadline)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "monotonic deadline overflow for duration " + std::to_string(duration.count())
            );
        }

        return *deadline;
    }

    auto elapsedNanosecondsSince(
        MonotonicInstant instant,
        MonotonicInstant origin
    ) noexcept -> std::uint64_t
    {
        using Duration = MonotonicInstant::Duration;
        using Period = Duration::period;

        auto const elapsed = instant.saturatingDurationSince(origin);
        if constexpr (std::ratio_equal_v<Period, std::nano>)
        {
            return checkedCast<std::uint64_t>(elapsed.count()).value_or(
                std::numeric_limits<std::uint64_t>::max()
            );
        }
        else
        {
            using FloatingNanoseconds = std::chrono::duration<long double, std::nano>;
            auto const nanoseconds = FloatingNanoseconds{elapsed}.count();
            auto const upperExclusive = std::ldexp(
                1.0L,
                std::numeric_limits<std::uint64_t>::digits
            );
            if (!std::isfinite(nanoseconds) || nanoseconds >= upperExclusive)
            {
                return std::numeric_limits<std::uint64_t>::max();
            }

            return checkedIntegralCast<std::uint64_t>(std::floor(nanoseconds)).value_or(
                std::numeric_limits<std::uint64_t>::max()
            );
        }
    }
}
