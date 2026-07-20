#include "pacing.hpp"

#include "log-jsonl.hpp"
#include "shutdown.hpp"

#include <core/numeric/checked-cast.hpp>
#include <core/time/monotonic-time.hpp>
#include <domain/error.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <thread>
#include <utility>

namespace
{
    constexpr auto sleepStep = std::chrono::milliseconds{20};

    [[nodiscard]]
    auto interruptibleSleep(std::uint64_t totalMilliseconds) -> bool
    {
        auto const started = uf::MonotonicInstant::now();
        while (true)
        {
            if (uf::m0_demo::stopRequested())
            {
                return false;
            }

            auto const elapsed = uf::MonotonicInstant::now().saturatingDurationSince(started);
            auto const elapsedMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                elapsed
            );
            auto const elapsedCount = uf::checkedCast<std::uint64_t>(
                elapsedMilliseconds.count()
            ).value_or(0U);
            if (elapsedCount >= totalMilliseconds)
            {
                return true;
            }

            auto const remaining = totalMilliseconds - elapsedCount;
            auto const stepCount = std::min<std::uint64_t>(
                remaining,
                static_cast<std::uint64_t>(sleepStep.count())
            );
            auto const step = uf::checkedCast<std::chrono::milliseconds::rep>(stepCount);
            if (!step)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{*step});
        }
    }
}

namespace uf::m0_demo
{
    auto ClickDelay::create(
        std::uint64_t minimumMilliseconds,
        std::uint64_t maximumMilliseconds
    ) -> Result<ClickDelay>
    {
        if (minimumMilliseconds == 0U || maximumMilliseconds == 0U)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "click delay bounds must be positive, got {}-{} ms",
                    minimumMilliseconds,
                    maximumMilliseconds
                )
            );
        }
        if (minimumMilliseconds > maximumMilliseconds)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "click delay min {} ms exceeds max {} ms",
                    minimumMilliseconds,
                    maximumMilliseconds
                )
            );
        }

        return ClickDelay{minimumMilliseconds, maximumMilliseconds};
    }

    auto SplitMix64::next() noexcept -> std::uint64_t
    {
        m_state += std::uint64_t{0x9E37'79B9'7F4A'7C15};
        auto value = m_state;
        value = (value ^ (value >> 30U)) * std::uint64_t{0xBF58'476D'1CE4'E5B9};
        value = (value ^ (value >> 27U)) * std::uint64_t{0x94D0'49BB'1331'11EB};
        return value ^ (value >> 31U);
    }

    ClickPacer::ClickPacer(
        std::optional<ClickDelay> delay,
        std::uint64_t seed
    ) noexcept
        : m_delay{delay}
        , m_random{seed}
    {
    }

    auto ClickPacer::pauseBeforeClick(
        std::string_view label,
        std::uint32_t loopIndex,
        JsonlLog& log
    ) -> Result<PaceOutcome>
    {
        if (!m_delay)
        {
            return PaceOutcome::Elapsed;
        }

        auto const pauseMilliseconds = m_delay->pickMilliseconds(m_random.next());
        auto const completed = interruptibleSleep(pauseMilliseconds);
        UF_TRY(
            log.write(
                LogLine{"action", "click_delay"}
                    .loopIndex(loopIndex)
                    .outcome(completed ? "ok" : "interrupted")
                    .detail(
                        std::format(
                            "paused {}ms before {} click (range {}-{}ms)",
                            pauseMilliseconds,
                            label,
                            m_delay->minimumMilliseconds(),
                            m_delay->maximumMilliseconds()
                        )
                    )
            )
        );
        return completed ? PaceOutcome::Elapsed : PaceOutcome::Stopped;
    }
}
