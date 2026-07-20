#pragma once

#include "core/numeric/checked-arithmetic.hpp"

#include <chrono>
#include <compare>
#include <concepts>
#include <optional>

namespace uf
{
    class MonotonicInstant final
    {
    public:
        using Clock = std::chrono::steady_clock;
        using Duration = Clock::duration;
        using TimePoint = Clock::time_point;

        static_assert(CheckedInteger<Duration::rep>);

    private:
        TimePoint m_timePoint;

        constexpr explicit MonotonicInstant(TimePoint timePoint) noexcept
            : m_timePoint{timePoint}
        {
        }

    public:
        auto operator<=>(MonotonicInstant const&) const -> std::strong_ordering = default;

        [[nodiscard]] static auto now() noexcept -> MonotonicInstant;

        [[nodiscard]]
        static constexpr auto fromTimePoint(TimePoint timePoint) noexcept -> MonotonicInstant
        {
            return MonotonicInstant{timePoint};
        }

        [[nodiscard]] constexpr auto timePoint() const noexcept -> TimePoint { return m_timePoint; }

        [[nodiscard]]
        constexpr auto checkedAdd(Duration duration) const noexcept -> std::optional<MonotonicInstant>
        {
            auto const count = uf::checkedAdd(
                m_timePoint.time_since_epoch().count(),
                duration.count()
            );
            if (!count)
            {
                return std::nullopt;
            }

            return MonotonicInstant{TimePoint{Duration{*count}}};
        }

        [[nodiscard]]
        constexpr auto saturatingDurationSince(MonotonicInstant earlier) const noexcept -> Duration
        {
            auto const currentCount = m_timePoint.time_since_epoch().count();
            auto const earlierCount = earlier.m_timePoint.time_since_epoch().count();
            if (currentCount <= earlierCount)
            {
                return Duration::zero();
            }

            auto const difference = uf::checkedSubtract(currentCount, earlierCount);
            if (!difference)
            {
                return Duration::max();
            }

            return Duration{*difference};
        }
    };

    inline auto MonotonicInstant::now() noexcept -> MonotonicInstant
    {
        return MonotonicInstant{Clock::now()};
    }
}
