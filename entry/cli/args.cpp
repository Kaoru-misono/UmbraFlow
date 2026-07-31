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
            auto constexpr flags = std::array<std::string_view, 8>{
                "--project",
                "--selector",
                "--task",
                "--budget",
                "--recognition-timeout",
                "--max-frame-age",
                "--trace",
                "--ocr-models",
            };
            return std::ranges::find(flags, flag) != flags.end();
        }

        // `drive` shares every recognition and delivery bound with `run` on purpose:
        // the two front-ends must not be able to run under different guarantees.
        // --task is absent and --queue/--results are present, which is the argument
        // shape refusing a session that is both at once.
        [[nodiscard]]
        auto isDriveValueFlag(std::string_view flag) noexcept -> bool
        {
            auto constexpr flags = std::array<std::string_view, 10>{
                "--project",
                "--selector",
                "--queue",
                "--results",
                "--budget",
                "--recognition-timeout",
                "--max-frame-age",
                "--idle-timeout",
                "--trace",
                "--ocr-models",
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
        auto task     = std::optional<std::string>{};

        auto budget             = k_defaultPixelComparisonBudget;
        auto recognitionTimeout = k_defaultRunRecognitionTimeout;
        auto maxFrameAge        = k_defaultRunMaxFrameAge;
        auto trace              = std::filesystem::path{k_defaultTracePath};
        auto ocrModels          = std::optional<std::filesystem::path>{};

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
            else if (flag == "--task")
            {
                task = value;
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
            else if (flag == "--ocr-models")
            {
                ocrModels = std::filesystem::path{value};
            }
            index += 2U;
        }

        UF_TRY_VALUE(requiredProject, require(std::move(project), "--project"));
        UF_TRY_VALUE(requiredSelector, require(std::move(selector), "--selector"));
        UF_TRY_VALUE(requiredTask, require(std::move(task), "--task"));

        return RunArgs{
            .project            = std::move(requiredProject),
            .selector           = std::move(requiredSelector),
            .task               = std::move(requiredTask),
            .budget             = budget,
            .recognitionTimeout = recognitionTimeout,
            .maxFrameAge        = maxFrameAge,
            .trace              = std::move(trace),
            .ocrModels          = std::move(ocrModels),
        };
    }

    auto parseDriveArguments(std::span<std::string const> raw) -> Result<DriveArgs>
    {
        auto project  = std::optional<std::filesystem::path>{};
        auto selector = std::optional<std::string>{};
        auto queue    = std::optional<std::filesystem::path>{};
        auto results  = std::optional<std::filesystem::path>{};

        auto budget             = k_defaultPixelComparisonBudget;
        auto recognitionTimeout = k_defaultRunRecognitionTimeout;
        auto maxFrameAge        = k_defaultRunMaxFrameAge;
        auto idleTimeout        = k_defaultDriveIdleTimeout;
        auto trace              = std::filesystem::path{k_defaultTracePath};
        auto ocrModels          = std::optional<std::filesystem::path>{};

        auto index = std::size_t{0};
        while (index < raw.size())
        {
            auto const& flag = raw[index];
            if (!isDriveValueFlag(flag))
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
            else if (flag == "--queue")
            {
                queue = std::filesystem::path{value};
            }
            else if (flag == "--results")
            {
                results = std::filesystem::path{value};
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
            else if (flag == "--idle-timeout")
            {
                UF_TRY_VALUE(count, parseUnsigned(value, flag));
                UF_TRY_VALUE(
                    parsed,
                    parseDurationCount<std::chrono::seconds>(count, flag)
                );
                idleTimeout = parsed;
            }
            else if (flag == "--trace")
            {
                trace = std::filesystem::path{value};
            }
            else if (flag == "--ocr-models")
            {
                ocrModels = std::filesystem::path{value};
            }
            index += 2U;
        }

        UF_TRY_VALUE(requiredProject, require(std::move(project), "--project"));
        UF_TRY_VALUE(requiredSelector, require(std::move(selector), "--selector"));
        UF_TRY_VALUE(requiredQueue, require(std::move(queue), "--queue"));
        UF_TRY_VALUE(requiredResults, require(std::move(results), "--results"));

        return DriveArgs{
            .project            = std::move(requiredProject),
            .selector           = std::move(requiredSelector),
            .queue              = std::move(requiredQueue),
            .results            = std::move(requiredResults),
            .budget             = budget,
            .recognitionTimeout = recognitionTimeout,
            .maxFrameAge        = maxFrameAge,
            .idleTimeout        = idleTimeout,
            .trace              = std::move(trace),
            .ocrModels          = std::move(ocrModels),
        };
    }

    auto runUsageText() noexcept -> std::string_view
    {
        return
            "Usage:\n"
            "  umbra-flow run --project DIR --selector TITLE-SUBSTRING "
            "--task NAME [options]\n"
            "\n"
            "Required:\n"
            "  --project DIR                Published annotation project directory\n"
            "  --selector TITLE-SUBSTRING   Substring of the target window title\n"
            "  --task NAME                  Run project task tasks/NAME.luau\n"
            "\n"
            "Options:\n"
            "  --budget N                   Pixel comparison ceiling per recognition\n"
            "  --recognition-timeout MS     Per-recognition deadline; default: 2000\n"
            "  --max-frame-age MS           Action frame age ceiling; default: 750\n"
            "  --trace PATH                 Trace JSONL path; default: "
            "umbra-flow-trace.jsonl\n"
            "  --ocr-models DIR             \"models\" directory enabling cycle_read;\n"
            "                                default: no OCR engine, cycle_read refuses\n";
    }

    auto driveUsageText() noexcept -> std::string_view
    {
        return
            "Usage:\n"
            "  umbra-flow drive --project DIR --selector TITLE-SUBSTRING "
            "--queue PATH --results PATH [options]\n"
            "\n"
            "Executes operator commands arriving as JSON lines appended to --queue,\n"
            "writing one JSON result line per command to --results. Never runs a\n"
            "task: `run` and `drive` cannot share one session.\n"
            "\n"
            "Required:\n"
            "  --project DIR                Published annotation project directory\n"
            "  --selector TITLE-SUBSTRING   Substring of the target window title\n"
            "  --queue PATH                 Command queue this session tails\n"
            "  --results PATH               Result lines; must not already exist\n"
            "\n"
            "Options:\n"
            "  --budget N                   Pixel comparison ceiling per recognition\n"
            "  --recognition-timeout MS     Per-recognition deadline; default: 2000\n"
            "  --max-frame-age MS           Action frame age ceiling; default: 750\n"
            "  --idle-timeout S             End after an idle queue; default: 120\n"
            "  --trace PATH                 Trace JSONL path; default: "
            "umbra-flow-trace.jsonl\n"
            "  --ocr-models DIR             \"models\" directory enabling cycle_read;\n"
            "                                default: no OCR engine, cycle_read refuses\n";
    }

    auto usageText() -> std::string
    {
        auto text = std::string{runUsageText()};
        text += '\n';
        text += driveUsageText();
        return text;
    }
}
