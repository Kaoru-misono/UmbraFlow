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
            auto constexpr flags = std::array<std::string_view, 9>{
                "--project",
                "--selector",
                "--task",
                "--max-runtime",
                "--budget",
                "--recognition-timeout",
                "--max-frame-age",
                "--trace",
                "--ocr-models",
            };
            return std::ranges::find(flags, flag) != flags.end();
        }

        // --task is absent and --queue/--results are present, which is the
        // argument shape refusing a session that is both at once. See
        // ExploreArgs.
        [[nodiscard]]
        auto isExploreValueFlag(std::string_view flag) noexcept -> bool
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

        // The absence of --selector is the argument shape stating what the matrix
        // is: authored screens against authored marks, with no live frame in it.
        // See CheckArgs.
        [[nodiscard]]
        auto isCheckValueFlag(std::string_view flag) noexcept -> bool
        {
            auto constexpr flags = std::array<std::string_view, 5>{
                "--project",
                "--budget",
                "--recognition-timeout",
                "--trace",
                "--ocr-models",
            };
            return std::ranges::find(flags, flag) != flags.end();
        }

        // The one flag in this CLI that takes no value: it turns a measurement on
        // and there is nothing to say about it beyond that. Kept a separate
        // predicate rather than a value flag reading "true"/"false", because
        // `--sweep-pages false` would be a spelling that reads like it disables
        // something and one typo away from enabling it.
        [[nodiscard]]
        auto isCheckSwitch(std::string_view flag) noexcept -> bool
        {
            return flag == "--sweep-pages";
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
        auto maxRuntime         = k_defaultRunMaxRuntime;
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
            else if (flag == "--max-runtime")
            {
                UF_TRY_VALUE(count, parseUnsigned(value, flag));
                // Zero is not a shorter ceiling: the run's deadline would be its
                // own start instant and the task would be cut at the first
                // safepoint, so it is refused rather than read as "no limit".
                if (count == 0U)
                {
                    return invalid(
                        std::format(
                            "{} expects a positive millisecond count, got \"{}\"",
                            flag,
                            value
                        )
                    );
                }
                UF_TRY_VALUE(
                    parsed,
                    parseDurationCount<std::chrono::milliseconds>(count, flag)
                );
                maxRuntime = parsed;
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
            .maxRuntime         = maxRuntime,
            .trace              = std::move(trace),
            .ocrModels          = std::move(ocrModels),
        };
    }

    auto parseExploreArguments(std::span<std::string const> raw) -> Result<ExploreArgs>
    {
        auto project  = std::optional<std::filesystem::path>{};
        auto selector = std::optional<std::string>{};
        auto queue    = std::optional<std::filesystem::path>{};
        auto results  = std::optional<std::filesystem::path>{};

        auto budget             = k_defaultPixelComparisonBudget;
        auto recognitionTimeout = k_defaultRunRecognitionTimeout;
        auto maxFrameAge        = k_defaultRunMaxFrameAge;
        auto idleTimeout        = k_defaultExploreIdleTimeout;
        auto trace              = std::filesystem::path{k_defaultTracePath};
        auto ocrModels          = std::optional<std::filesystem::path>{};

        auto index = std::size_t{0};
        while (index < raw.size())
        {
            auto const& flag = raw[index];
            if (!isExploreValueFlag(flag))
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

        return ExploreArgs{
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

    auto parseCheckArguments(std::span<std::string const> raw) -> Result<CheckArgs>
    {
        auto project = std::optional<std::filesystem::path>{};

        auto budget             = k_defaultPixelComparisonBudget;
        auto recognitionTimeout = k_defaultRunRecognitionTimeout;
        auto trace              = std::filesystem::path{k_defaultCheckTracePath};
        auto ocrModels          = std::optional<std::filesystem::path>{};
        auto sweepPages         = false;

        auto index = std::size_t{0};
        while (index < raw.size())
        {
            auto const& flag = raw[index];
            if (isCheckSwitch(flag))
            {
                sweepPages = true;
                index += 1U;
                continue;
            }
            if (!isCheckValueFlag(flag))
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

        return CheckArgs{
            .project            = std::move(requiredProject),
            .budget             = budget,
            .recognitionTimeout = recognitionTimeout,
            .trace              = std::move(trace),
            .ocrModels          = std::move(ocrModels),
            .sweepPages         = sweepPages,
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
            "  --max-runtime MS             Ceiling on the WHOLE run: a task is one\n"
            "                                unit of script; default: 30 minutes\n"
            "  --trace PATH                 Trace JSONL path; default: "
            "umbra-flow-trace.jsonl\n"
            "  --ocr-models DIR             \"models\" directory enabling the text\n"
            "                                reads; default: no engine, both read verbs\n"
            "                                refuse\n";
    }

    auto exploreUsageText() noexcept -> std::string_view
    {
        return
            "Usage:\n"
            "  umbra-flow explore --project DIR --selector TITLE-SUBSTRING "
            "--queue PATH --results PATH [options]\n"
            "\n"
            "Executes Luau chunks arriving as JSON lines appended to --queue, one\n"
            "per line as {\"id\":\"...\",\"chunk\":\"...\"}, writing one JSON result\n"
            "line per chunk to --results. The chunks run in the exploration\n"
            "environment: every verb a task has, plus a bare-coordinate click, a\n"
            "region crop and a pixel probe. Never runs a task.\n"
            "\n"
            "Required:\n"
            "  --project DIR                Annotation project directory\n"
            "  --selector TITLE-SUBSTRING   Substring of the target window title\n"
            "  --queue PATH                 Chunk queue this session tails\n"
            "  --results PATH               Result lines; must not already exist\n"
            "\n"
            "Options:\n"
            "  --budget N                   Pixel comparison ceiling per recognition\n"
            "  --recognition-timeout MS     Per-recognition deadline; default: 2000\n"
            "  --max-frame-age MS           Action frame age ceiling; default: 750\n"
            "  --idle-timeout S             End after an idle queue; default: 120\n"
            "  --trace PATH                 Trace JSONL path; default: "
            "umbra-flow-trace.jsonl\n"
            "  --ocr-models DIR             \"models\" directory enabling the text\n"
            "                                reads; default: no engine, both read verbs\n"
            "                                refuse\n";
    }

    auto checkUsageText() noexcept -> std::string_view
    {
        return
            "Usage:\n"
            "  umbra-flow check --project DIR [options]\n"
            "\n"
            "Measures every appearance the project's page model declares against\n"
            "every screen it holds under assets/screens, judges each cell against\n"
            "the expectations recorded in the project file, and writes the verdict\n"
            "as JSON lines. Exits non-zero when the verdict is not accepted.\n"
            "\n"
            "It binds no target: a frame captured to measure against is not a\n"
            "screen the model was authored on.\n"
            "\n"
            "Required:\n"
            "  --project DIR                Published annotation project directory\n"
            "\n"
            "Options:\n"
            "  --budget N                   Pixel comparison ceiling per search\n"
            "  --recognition-timeout MS     Per-search deadline; default: 2000\n"
            "  --trace PATH                 Trace JSONL path; default: "
            "umbra-flow-check-trace.jsonl\n"
            "  --ocr-models DIR             \"models\" directory enabling the text\n"
            "                                reads; required when the project claims\n"
            "                                what a region reads, refused without it\n"
            "  --sweep-pages                Also resolve EVERY declared page on\n"
            "                                every screen and report what co-resolved;\n"
            "                                a measurement, never a finding, and the\n"
            "                                bulk of the wall clock on a real corpus\n";
    }

    auto parseReplayArguments(std::span<std::string const> raw)
        -> Result<ReplayArgs>
    {
        auto project = std::optional<std::filesystem::path>{};
        auto trace   = std::optional<std::filesystem::path>{};

        auto index = std::size_t{0};
        while (index < raw.size())
        {
            auto const& flag = raw[index];
            if (flag != "--project" && flag != "--trace")
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
            else
            {
                trace = std::filesystem::path{value};
            }
            index += 2U;
        }

        // Both required and neither defaulted. A default project would check a
        // run against whatever model happened to sit under the working
        // directory, and a default trace would replay whichever run was written
        // there last -- while a replay names exactly one of each by
        // construction.
        UF_TRY_VALUE(requiredProject, require(std::move(project), "--project"));
        UF_TRY_VALUE(requiredTrace, require(std::move(trace), "--trace"));

        return ReplayArgs{
            .project = std::move(requiredProject),
            .trace   = std::move(requiredTrace),
        };
    }

    auto replayUsageText() noexcept -> std::string_view
    {
        return
            "Usage:\n"
            "  umbra-flow replay --project DIR --trace PATH\n"
            "\n"
            "Reads one recorded run and reports every page it moved between\n"
            "against the edges the project's page model draws, as JSON lines.\n"
            "Exits non-zero when the verdict is not accepted.\n"
            "\n"
            "It binds no target and opens no frame: everything it judges was\n"
            "measured when the run happened.\n"
            "\n"
            "Refused before anything is judged: a trace recorded by a front end\n"
            "that delivers no input, and a trace whose page model is not the one\n"
            "on disk now.\n"
            "\n"
            "Required:\n"
            "  --project DIR                Published annotation project directory\n"
            "  --trace PATH                 A run's trace JSONL, as an INPUT\n";
    }

    auto usageText() -> std::string
    {
        auto text = std::string{runUsageText()};
        text += '\n';
        text += exploreUsageText();
        text += '\n';
        text += checkUsageText();
        text += '\n';
        text += replayUsageText();
        return text;
    }
}
