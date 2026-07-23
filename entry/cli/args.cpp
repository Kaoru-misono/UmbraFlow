#include "args.hpp"

#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace uf::cli
{
    namespace
    {
        [[nodiscard]]
        auto isRunValueFlag(std::string_view flag) noexcept -> bool
        {
            auto constexpr flags = std::array<std::string_view, 10>{
                "--project",
                "--selector",
                "--page",
                "--action",
                "--timeout",
                "--poll",
                "--budget",
                "--recognition-timeout",
                "--max-frame-age",
                "--trace",
            };
            return std::ranges::find(flags, flag) != flags.end();
        }

        [[nodiscard]]
        auto invalid(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        [[nodiscard]]
        auto parseUnsigned(
            std::string_view value,
            std::string_view flag
        ) -> Result<uint64>
        {
            auto parsed             = uint64{};
            auto const* const begin = std::to_address(value.begin());
            auto const* const end   = std::to_address(value.end());
            auto const result       = std::from_chars(begin, end, parsed);
            if (result.ec != std::errc{} || result.ptr != end)
            {
                return invalid(
                    std::format("{} expects an integer, got \"{}\"", flag, value)
                );
            }
            return parsed;
        }

        [[nodiscard]]
        auto parseMilliseconds(
            std::string_view value,
            std::string_view flag
        ) -> Result<MonotonicInstant::Duration>
        {
            UF_TRY_VALUE(milliseconds, parseUnsigned(value, flag));
            using Milliseconds = std::chrono::milliseconds;
            using Duration     = MonotonicInstant::Duration;
            auto const maximum = std::chrono::duration_cast<Milliseconds>(
                Duration::max()
            );
            auto const maximumCount = checkedCast<uint64>(maximum.count());
            auto const count        = checkedCast<Milliseconds::rep>(milliseconds);
            if (!maximumCount || milliseconds > *maximumCount || !count)
            {
                return invalid(
                    std::format("{} millisecond count is too large", flag)
                );
            }
            return std::chrono::duration_cast<Duration>(Milliseconds{*count});
        }

        // --poll bounds. 0 would spin the wait loop with no delay between frames;
        // a value above a minute would leave a Ctrl-C unacknowledged for longer
        // than an operator will tolerate, since the wait only re-checks the token
        // across poll sleeps.
        constexpr auto g_minPollMilliseconds = uint64{1};
        constexpr auto g_maxPollMilliseconds = uint64{60'000};

        [[nodiscard]]
        auto parsePollInterval(
            std::string_view value,
            std::string_view flag
        ) -> Result<MonotonicInstant::Duration>
        {
            UF_TRY_VALUE(milliseconds, parseUnsigned(value, flag));
            if (
                milliseconds < g_minPollMilliseconds
                || milliseconds > g_maxPollMilliseconds
            )
            {
                return invalid(
                    std::format(
                        "{} must be between {} and {} ms, got {}",
                        flag,
                        g_minPollMilliseconds,
                        g_maxPollMilliseconds,
                        milliseconds
                    )
                );
            }
            return parseMilliseconds(value, flag);
        }

        [[nodiscard]]
        auto parseSeconds(
            std::string_view value,
            std::string_view flag
        ) -> Result<MonotonicInstant::Duration>
        {
            UF_TRY_VALUE(seconds, parseUnsigned(value, flag));
            using Seconds  = std::chrono::seconds;
            using Duration = MonotonicInstant::Duration;
            auto const maximum      = std::chrono::duration_cast<Seconds>(Duration::max());
            auto const maximumCount = checkedCast<uint64>(maximum.count());
            auto const count        = checkedCast<Seconds::rep>(seconds);
            if (!maximumCount || seconds > *maximumCount || !count)
            {
                return invalid(std::format("{} second count is too large", flag));
            }
            return std::chrono::duration_cast<Duration>(Seconds{*count});
        }

        template <typename Value>
        [[nodiscard]]
        auto require(
            std::optional<Value> value,
            std::string_view flag
        ) -> Result<Value>
        {
            if (!value || value->empty())
            {
                return invalid(std::format("missing required argument {}", flag));
            }
            return *std::move(value);
        }
    }

    auto parseRunArguments(std::span<std::string const> raw) -> Result<RunArgs>
    {
        auto project  = std::optional<std::filesystem::path>{};
        auto selector = std::optional<std::string>{};
        auto page     = std::optional<std::string>{};
        auto action   = std::optional<std::string>{};

        auto timeout            = g_defaultRunTimeout;
        auto pollInterval       = g_defaultRunPollInterval;
        auto budget             = g_defaultPixelComparisonBudget;
        auto recognitionTimeout = g_defaultRunRecognitionTimeout;
        auto maxFrameAge        = g_defaultRunMaxFrameAge;
        auto trace              = std::filesystem::path{g_defaultTracePath};

        auto index = std::size_t{0};
        while (index < raw.size())
        {
            auto const& flag = raw[index];
            if (!isRunValueFlag(flag))
            {
                return invalid(std::format("unknown argument \"{}\"", flag));
            }
            if (index + 1U >= raw.size())
            {
                return invalid(std::format("missing value for {}", flag));
            }
            auto const& value = raw[index + 1U];

            if (flag == "--project")
            {
                project = std::filesystem::path{value};
            }
            else if (flag == "--selector")
            {
                selector = value;
            }
            else if (flag == "--page")
            {
                page = value;
            }
            else if (flag == "--action")
            {
                action = value;
            }
            else if (flag == "--timeout")
            {
                UF_TRY_VALUE(parsed, parseSeconds(value, flag));
                timeout = parsed;
            }
            else if (flag == "--poll")
            {
                UF_TRY_VALUE(parsed, parsePollInterval(value, flag));
                pollInterval = parsed;
            }
            else if (flag == "--budget")
            {
                UF_TRY_VALUE(parsed, parseUnsigned(value, flag));
                budget = parsed;
            }
            else if (flag == "--recognition-timeout")
            {
                UF_TRY_VALUE(parsed, parseMilliseconds(value, flag));
                recognitionTimeout = parsed;
            }
            else if (flag == "--max-frame-age")
            {
                UF_TRY_VALUE(parsed, parseMilliseconds(value, flag));
                maxFrameAge = parsed;
            }
            else if (flag == "--trace")
            {
                trace = std::filesystem::path{value};
            }
            index += 2U;
        }

        UF_TRY_VALUE(requiredProject, require(std::move(project), "--project"));
        UF_TRY_VALUE(requiredSelector, require(std::move(selector), "--selector"));
        UF_TRY_VALUE(requiredPage, require(std::move(page), "--page"));
        UF_TRY_VALUE(requiredAction, require(std::move(action), "--action"));

        return RunArgs{
            .m_project            = std::move(requiredProject),
            .m_selector           = std::move(requiredSelector),
            .m_page               = std::move(requiredPage),
            .m_action             = std::move(requiredAction),
            .m_timeout            = timeout,
            .m_pollInterval       = pollInterval,
            .m_budget             = budget,
            .m_recognitionTimeout = recognitionTimeout,
            .m_maxFrameAge        = maxFrameAge,
            .m_trace              = std::move(trace),
        };
    }

    auto runUsageText() noexcept -> std::string_view
    {
        return
            "Usage: umbra-flow run --project DIR --selector TITLE-SUBSTRING "
            "--page NAME --action NAME [options]\n"
            "\n"
            "Required:\n"
            "  --project DIR                Published annotation project directory\n"
            "  --selector TITLE-SUBSTRING   Substring of the target window title\n"
            "  --page NAME                  Page recognizer name to wait for\n"
            "  --action NAME                Action target name to click\n"
            "\n"
            "Options:\n"
            "  --timeout SEC                waitForPage timeout; default: 30\n"
            "  --poll MS                    Poll interval; default: 250\n"
            "  --budget N                   Pixel comparison ceiling per recognition\n"
            "  --recognition-timeout MS     Per-recognition deadline; default: 2000\n"
            "  --max-frame-age MS           Action frame age ceiling; default: 750\n"
            "  --trace PATH                 Trace JSONL path; default: "
            "umbra-flow-trace.jsonl\n";
    }
}
