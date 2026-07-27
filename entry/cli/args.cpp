#include "args.hpp"

#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <concepts>
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
            auto constexpr flags = std::array<std::string_view, 11>{
                "--project",
                "--selector",
                "--page",
                "--action",
                "--task",
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

        template <typename Unit>
        [[nodiscard]]
        auto parseDurationCount(
            uint64 count,
            std::string_view flag
        ) -> Result<MonotonicInstant::Duration>
        {
            using Duration = MonotonicInstant::Duration;
            auto const maximum      = std::chrono::duration_cast<Unit>(Duration::max());
            auto const maximumCount = checkedCast<uint64>(maximum.count());
            auto const unitCount    = checkedCast<typename Unit::rep>(count);
            if (!maximumCount || count > *maximumCount || !unitCount)
            {
                if constexpr (std::same_as<Unit, std::chrono::milliseconds>)
                {
                    return invalid(
                        std::format("{} millisecond count is too large", flag)
                    );
                }
                else
                {
                    static_assert(std::same_as<Unit, std::chrono::seconds>);
                    return invalid(
                        std::format("{} second count is too large", flag)
                    );
                }
            }
            return std::chrono::duration_cast<Duration>(Unit{*unitCount});
        }

        // --poll bounds. 0 would spin the wait loop with no delay between frames;
        // a value above a minute would leave a Ctrl-C unacknowledged for longer
        // than an operator will tolerate, since the wait only re-checks the token
        // across poll sleeps.
        constexpr auto k_minPollMilliseconds = uint64{1};
        constexpr auto k_maxPollMilliseconds = uint64{60'000};

        [[nodiscard]]
        auto parsePollInterval(
            std::string_view value,
            std::string_view flag
        ) -> Result<MonotonicInstant::Duration>
        {
            UF_TRY_VALUE(milliseconds, parseUnsigned(value, flag));
            if (
                milliseconds < k_minPollMilliseconds
                || milliseconds > k_maxPollMilliseconds
            )
            {
                return invalid(
                    std::format(
                        "{} must be between {} and {} ms, got {}",
                        flag,
                        k_minPollMilliseconds,
                        k_maxPollMilliseconds,
                        milliseconds
                    )
                );
            }
            return parseDurationCount<std::chrono::milliseconds>(milliseconds, flag);
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
        auto task     = std::optional<std::string>{};

        auto timeout            = k_defaultRunTimeout;
        auto pollInterval       = k_defaultRunPollInterval;
        auto budget             = k_defaultPixelComparisonBudget;
        auto recognitionTimeout = k_defaultRunRecognitionTimeout;
        auto maxFrameAge        = k_defaultRunMaxFrameAge;
        auto trace              = std::filesystem::path{k_defaultTracePath};

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
            else if (flag == "--task")
            {
                task = value;
            }
            else if (flag == "--timeout")
            {
                UF_TRY_VALUE(count, parseUnsigned(value, flag));
                UF_TRY_VALUE(
                    parsed,
                    parseDurationCount<std::chrono::seconds>(count, flag)
                );
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
                UF_TRY_VALUE(count, parseUnsigned(value, flag));
                UF_TRY_VALUE(
                    parsed,
                    parseDurationCount<std::chrono::milliseconds>(count, flag)
                );
                recognitionTimeout = parsed;
            }
            else if (flag == "--max-frame-age")
            {
                UF_TRY_VALUE(count, parseUnsigned(value, flag));
                UF_TRY_VALUE(
                    parsed,
                    parseDurationCount<std::chrono::milliseconds>(count, flag)
                );
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

        auto const haveTask   = task && !task->empty();
        auto const havePage   = page && !page->empty();
        auto const haveAction = action && !action->empty();

        // --task selects the script path; --page with --action selects the
        // single-step smoke path. The two are mutually exclusive: a task script
        // sources its own page and action names, so accepting both would make it
        // ambiguous which path the run intends.
        if (haveTask && (havePage || haveAction))
        {
            return invalid("--task cannot be combined with --page or --action");
        }

        if (haveTask)
        {
            return RunArgs{
                .project            = std::move(requiredProject),
                .selector           = std::move(requiredSelector),
                .page               = {},
                .action             = {},
                .task               = *std::move(task),
                .timeout            = timeout,
                .pollInterval       = pollInterval,
                .budget             = budget,
                .recognitionTimeout = recognitionTimeout,
                .maxFrameAge        = maxFrameAge,
                .trace              = std::move(trace),
            };
        }

        // The smoke path still requires an explicit page and action. Omitting all
        // of --task, --page, and --action lands here and reports the missing page,
        // preserving the pre-task usage contract that page and action are required.
        UF_TRY_VALUE(requiredPage, require(std::move(page), "--page"));
        UF_TRY_VALUE(requiredAction, require(std::move(action), "--action"));

        return RunArgs{
            .project            = std::move(requiredProject),
            .selector           = std::move(requiredSelector),
            .page               = std::move(requiredPage),
            .action             = std::move(requiredAction),
            .task               = {},
            .timeout            = timeout,
            .pollInterval       = pollInterval,
            .budget             = budget,
            .recognitionTimeout = recognitionTimeout,
            .maxFrameAge        = maxFrameAge,
            .trace              = std::move(trace),
        };
    }

    auto runUsageText() noexcept -> std::string_view
    {
        return
            "Usage:\n"
            "  umbra-flow run --project DIR --selector TITLE-SUBSTRING "
            "--task NAME [options]\n"
            "  umbra-flow run --project DIR --selector TITLE-SUBSTRING "
            "--page NAME --action NAME [options]\n"
            "\n"
            "Required:\n"
            "  --project DIR                Published annotation project directory\n"
            "  --selector TITLE-SUBSTRING   Substring of the target window title\n"
            "\n"
            "One run mode is required:\n"
            "  --task NAME                  Run project task tasks/NAME.luau\n"
            "  --page NAME --action NAME    Wait for a page and click an action\n"
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
