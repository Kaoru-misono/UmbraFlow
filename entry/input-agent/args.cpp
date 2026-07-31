#include "args.hpp"

#include "arg-parsing.hpp"

#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace uf::input_agent
{
    namespace
    {
        using detail::invalid;
        using detail::parseInteger;
        using detail::parseWindowHandle;
        using detail::require;

        [[nodiscard]]
        auto isInputAgentValueFlag(std::string_view flag) noexcept -> bool
        {
            auto constexpr flags = std::array<std::string_view, 6>{
                "--hwnd",
                "--queue",
                "--results",
                "--output-dir",
                "--idle-timeout-s",
                "--queue-start",
            };
            return std::ranges::find(flags, flag) != flags.end();
        }

        struct QueueStartName final
        {
            std::string_view     name{};
            InputAgentQueueStart value{};
        };

        [[nodiscard]]
        auto parseQueueStart(
            std::string_view value
        ) -> Result<InputAgentQueueStart>
        {
            auto constexpr names = std::array<QueueStartName, 3>{{
                {.name = "refuse", .value = InputAgentQueueStart::Refuse},
                {.name = "beginning", .value = InputAgentQueueStart::Beginning},
                {.name = "end", .value = InputAgentQueueStart::End},
            }};
            auto const found = std::ranges::find(
                names,
                value,
                &QueueStartName::name
            );
            if (found == names.end())
            {
                return invalid(
                    std::format(
                        "--queue-start expects 'refuse', 'beginning', or "
                        "'end', got \"{}\"",
                        value
                    )
                );
            }
            return found->value;
        }

        [[nodiscard]]
        auto parsePositiveSeconds(
            std::string_view value,
            std::string_view flag
        ) -> Result<MonotonicInstant::Duration>
        {
            UF_TRY_VALUE(seconds, parseInteger<uint64>(value, flag));
            if (seconds == 0U)
            {
                return invalid(
                    std::format("{} must be a positive second count", flag)
                );
            }

            using Seconds = std::chrono::seconds;
            using Duration = MonotonicInstant::Duration;
            auto const maximum = std::chrono::duration_cast<Seconds>(Duration::max());
            auto const maximumCount = checkedCast<uint64>(maximum.count());
            if (!maximumCount || seconds > *maximumCount)
            {
                return invalid(std::format("{} second count is too large", flag));
            }
            auto const count = checkedCast<Seconds::rep>(seconds);
            if (!count)
            {
                return invalid(std::format("{} second count is too large", flag));
            }
            return std::chrono::duration_cast<Duration>(Seconds{*count});
        }
    }

    auto parseInputAgentArguments(
        std::span<std::string const> raw
    ) -> Result<InputAgentArgs>
    {
        auto windowHandle    = std::optional<intptr>{};
        auto queue           = std::optional<std::filesystem::path>{};
        auto results         = std::optional<std::filesystem::path>{};
        auto outputDirectory = std::optional<std::filesystem::path>{};
        auto idleTimeout     = k_defaultInputAgentIdleTimeout;
        auto queueStart      = InputAgentQueueStart::Refuse;

        auto index = std::size_t{0};
        while (index < raw.size())
        {
            auto const& flag = raw[index];
            if (!isInputAgentValueFlag(flag))
            {
                return invalid(
                    std::format("unknown input-agent argument \"{}\"", flag)
                );
            }
            if (index + 1U >= raw.size())
            {
                return invalid(std::format("missing value for {}", flag));
            }
            auto const& value = raw[index + 1U];

            if (flag == "--hwnd")
            {
                UF_TRY_VALUE(parsed, parseWindowHandle(value, flag));
                windowHandle = parsed;
            }
            else if (flag == "--queue")
            {
                queue = std::filesystem::path{value};
            }
            else if (flag == "--results")
            {
                results = std::filesystem::path{value};
            }
            else if (flag == "--output-dir")
            {
                outputDirectory = std::filesystem::path{value};
            }
            else if (flag == "--idle-timeout-s")
            {
                UF_TRY_VALUE(parsed, parsePositiveSeconds(value, flag));
                idleTimeout = parsed;
            }
            else if (flag == "--queue-start")
            {
                UF_TRY_VALUE(parsed, parseQueueStart(value));
                queueStart = parsed;
            }
            index += 2U;
        }

        UF_TRY_VALUE(requiredWindowHandle, require(windowHandle, "--hwnd"));
        UF_TRY_VALUE(requiredQueue, require(std::move(queue), "--queue"));
        UF_TRY_VALUE(requiredResults, require(std::move(results), "--results"));
        UF_TRY_VALUE(
            requiredOutputDirectory,
            require(std::move(outputDirectory), "--output-dir")
        );
        if (requiredQueue.empty())
        {
            return invalid("--queue must not be empty");
        }
        if (requiredResults.empty())
        {
            return invalid("--results must not be empty");
        }
        if (requiredOutputDirectory.empty())
        {
            return invalid("--output-dir must not be empty");
        }

        return InputAgentArgs{
            .windowHandle    = requiredWindowHandle,
            .queue           = std::move(requiredQueue),
            .results         = std::move(requiredResults),
            .outputDirectory = std::move(requiredOutputDirectory),
            .idleTimeout     = idleTimeout,
            .queueStart      = queueStart,
        };
    }

    auto inputAgentUsageText() noexcept -> std::string_view
    {
        return
            "Usage: umbra-input-agent --hwnd N|0xHEX --queue PATH "
            "--results PATH --output-dir DIR [options]\n"
            "\n"
            "The agent records how far it has consumed the queue in a\n"
            "<queue>.cursor file and resumes there, so a restart never\n"
            "re-delivers commands the target has already seen.\n"
            "\n"
            "Options:\n"
            "  --idle-timeout-s N           Default: 120; must be positive\n"
            "  --queue-start MODE           refuse|beginning|end; default:\n"
            "                               refuse. Read only when no cursor\n"
            "                               exists yet and the queue is not\n"
            "                               empty; a cursor always wins\n";
    }
}
