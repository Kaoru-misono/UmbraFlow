#include "pacing.hpp"

#include "log-jsonl.hpp"
#include "shutdown.hpp"

#include <core/numeric/checked-cast.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <thread>
#include <utility>

namespace uf::m0_demo
{
    namespace
    {
        constexpr auto g_sleepStep = std::chrono::milliseconds{20};

        [[nodiscard]]
        auto interruptibleSleep(uint64 totalMilliseconds) -> bool
        {
            auto const started = MonotonicInstant::now();
            while (true)
            {
                if (stopRequested())
                {
                    return false;
                }

                auto const elapsed = MonotonicInstant::now().saturatingDurationSince(started);
                auto const elapsedMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                    elapsed
                );
                auto const elapsedCount = checkedCast<uint64>(
                    elapsedMilliseconds.count()
                ).value_or(0U);
                if (elapsedCount >= totalMilliseconds)
                {
                    return true;
                }

                auto const remaining = totalMilliseconds - elapsedCount;
                auto const stepCount = std::min<uint64>(
                    remaining,
                    static_cast<uint64>(g_sleepStep.count())
                );
                auto const step = checkedCast<std::chrono::milliseconds::rep>(stepCount);
                if (!step)
                {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{*step});
            }
        }
    }

    auto ClickDelay::create(
        uint64 minimumMilliseconds,
        uint64 maximumMilliseconds
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

    auto SplitMix64::next() noexcept -> uint64
    {
        m_state += uint64{0x9E37'79B9'7F4A'7C15};
        auto value = m_state;
        value = (value ^ (value >> 30U)) * uint64{0xBF58'476D'1CE4'E5B9};
        value = (value ^ (value >> 27U)) * uint64{0x94D0'49BB'1331'11EB};
        return value ^ (value >> 31U);
    }

    ClickPacer::ClickPacer(
        std::optional<ClickDelay> delay,
        uint64 seed
    ) noexcept
        : m_delay{delay}
        , m_random{seed}
    {
    }

    auto ClickPacer::pauseBeforeClick(
        std::string_view label,
        uint32 loopIndex,
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
