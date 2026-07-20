#include "args.hpp"

#include <core/numeric/checked-cast.hpp>
#include <domain/error.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
    constexpr auto maximumAveragePixelSad = std::uint64_t{255};

    [[nodiscard]]
    auto isValueFlag(std::string_view flag) noexcept -> bool
    {
        auto constexpr flags = std::array<std::string_view, 18>{
            "--pid",
            "--hwnd",
            "--class",
            "--title",
            "--home-template",
            "--home-roi",
            "--result-template",
            "--result-roi",
            "--reset-template",
            "--reset-roi",
            "--threshold",
            "--mode",
            "--loops",
            "--max-action-frame-age",
            "--stall-timeout",
            "--click-delay-ms",
            "--seed",
            "--log",
        };
        return std::ranges::find(flags, flag) != flags.end();
    }

    [[nodiscard]]
    auto isSelectorFlag(std::string_view flag) noexcept -> bool
    {
        auto constexpr flags = std::array<std::string_view, 4>{
            "--pid",
            "--hwnd",
            "--class",
            "--title",
        };
        return std::ranges::find(flags, flag) != flags.end();
    }

    [[nodiscard]]
    auto isCaptureValueFlag(std::string_view flag) noexcept -> bool
    {
        auto constexpr flags = std::array<std::string_view, 7>{
            "--pid",
            "--hwnd",
            "--title",
            "--out",
            "--frames",
            "--interval-ms",
            "--log",
        };
        return std::ranges::find(flags, flag) != flags.end();
    }

    [[nodiscard]]
    auto isInputAgentValueFlag(std::string_view flag) noexcept -> bool
    {
        auto constexpr flags = std::array<std::string_view, 5>{
            "--hwnd",
            "--queue",
            "--results",
            "--output-dir",
            "--idle-timeout-s",
        };
        return std::ranges::find(flags, flag) != flags.end();
    }

    [[nodiscard]]
    auto invalid(std::string message) -> std::unexpected<uf::Error>
    {
        return uf::fail(
            uf::AutomationErrorKind::InvalidResource,
            std::move(message)
        );
    }

    template <std::integral Value>
    [[nodiscard]]
    auto parseInteger(
        std::string_view value,
        std::string_view flag,
        int base = 10
    ) -> uf::Result<Value>
    {
        auto const supplied = value;
        if (value.starts_with('+'))
        {
            value.remove_prefix(1);
        }
        auto parsed = Value{};
        auto const* const begin = std::to_address(value.begin());
        auto const* const end = std::to_address(value.end());
        auto const result = std::from_chars(
            begin,
            end,
            parsed,
            base
        );
        if (result.ec != std::errc{} || result.ptr != end)
        {
            return invalid(
                std::format(
                    "{} expects an integer, got \"{}\"",
                    flag,
                    supplied
                )
            );
        }

        return parsed;
    }

    [[nodiscard]]
    auto parseWindowHandle(
        std::string_view value,
        std::string_view flag
    ) -> uf::Result<std::intptr_t>
    {
        auto base = 10;
        if (value.starts_with("0x") || value.starts_with("0X"))
        {
            value.remove_prefix(2);
            base = 16;
        }

        UF_TRY_VALUE(parsed, parseInteger<std::int64_t>(value, flag, base));
        auto const converted = uf::checkedCast<std::intptr_t>(parsed);
        if (!converted)
        {
            return invalid(
                std::format(
                    "{} window handle is outside the machine-word range",
                    flag
                )
            );
        }
        return *converted;
    }

    [[nodiscard]]
    auto parseSelectorValue(
        uf::m0_demo::SelectorArgs& selector,
        std::string_view flag,
        std::string const& value
    ) -> uf::Status
    {
        if (flag == "--pid")
        {
            UF_TRY_VALUE(parsed, parseInteger<std::uint32_t>(value, flag));
            selector.m_process = parsed;
            return uf::ok();
        }
        if (flag == "--hwnd")
        {
            UF_TRY_VALUE(parsed, parseWindowHandle(value, flag));
            selector.m_windowHandle = parsed;
            return uf::ok();
        }
        if (flag == "--class")
        {
            selector.m_windowClass = value;
            return uf::ok();
        }
        if (flag == "--title")
        {
            selector.m_title = value;
            return uf::ok();
        }

        return invalid(std::format("unknown selector argument \"{}\"", flag));
    }

    [[nodiscard]]
    auto parseMode(std::string_view value) -> uf::Result<uf::m0_demo::Mode>
    {
        if (value == "guard")
        {
            return uf::m0_demo::Mode::Guard;
        }
        if (value == "coexist")
        {
            return uf::m0_demo::Mode::Coexist;
        }
        return invalid(
            std::format(
                "--mode expects 'guard' or 'coexist', got \"{}\"",
                value
            )
        );
    }

    [[nodiscard]]
    auto parseMilliseconds(
        std::string_view value,
        std::string_view flag
    ) -> uf::Result<uf::MonotonicInstant::Duration>
    {
        UF_TRY_VALUE(milliseconds, parseInteger<std::uint64_t>(value, flag));
        using Milliseconds = std::chrono::milliseconds;
        using Duration = uf::MonotonicInstant::Duration;
        auto const maximum = std::chrono::duration_cast<Milliseconds>(Duration::max());
        auto const maximumCount = uf::checkedCast<std::uint64_t>(maximum.count());
        if (!maximumCount || milliseconds > *maximumCount)
        {
            return invalid(
                std::format("{} millisecond count is too large", flag)
            );
        }

        auto const count = uf::checkedCast<Milliseconds::rep>(milliseconds);
        if (!count)
        {
            return invalid(
                std::format("{} millisecond count is too large", flag)
            );
        }

        return std::chrono::duration_cast<Duration>(Milliseconds{*count});
    }

    [[nodiscard]]
    auto parsePositiveDuration(
        std::string_view value,
        std::string_view flag
    ) -> uf::Result<uf::MonotonicInstant::Duration>
    {
        UF_TRY_VALUE(duration, parseMilliseconds(value, flag));
        if (duration == uf::MonotonicInstant::Duration::zero())
        {
            return invalid(
                std::format("{} must be a positive millisecond count", flag)
            );
        }
        return duration;
    }

    [[nodiscard]]
    auto parsePositiveSeconds(
        std::string_view value,
        std::string_view flag
    ) -> uf::Result<uf::MonotonicInstant::Duration>
    {
        UF_TRY_VALUE(seconds, parseInteger<std::uint64_t>(value, flag));
        if (seconds == 0U)
        {
            return invalid(
                std::format("{} must be a positive second count", flag)
            );
        }

        using Seconds = std::chrono::seconds;
        using Duration = uf::MonotonicInstant::Duration;
        auto const maximum = std::chrono::duration_cast<Seconds>(Duration::max());
        auto const maximumCount = uf::checkedCast<std::uint64_t>(maximum.count());
        if (!maximumCount || seconds > *maximumCount)
        {
            return invalid(std::format("{} second count is too large", flag));
        }
        auto const count = uf::checkedCast<Seconds::rep>(seconds);
        if (!count)
        {
            return invalid(std::format("{} second count is too large", flag));
        }
        return std::chrono::duration_cast<Duration>(Seconds{*count});
    }

    [[nodiscard]]
    auto trimmed(std::string_view value) noexcept -> std::string_view
    {
        auto constexpr whitespace = std::string_view{" \t\n\r\f\v"};
        auto const first = value.find_first_not_of(whitespace);
        if (first == std::string_view::npos)
        {
            return {};
        }
        auto const last = value.find_last_not_of(whitespace);
        return value.substr(first, last - first + 1U);
    }

    [[nodiscard]]
    auto parseFloat(
        std::string_view value,
        std::string_view flag
    ) -> uf::Result<float>
    {
        auto const normalized = trimmed(value);
        auto parsedText = normalized;
        if (parsedText.starts_with('+'))
        {
            parsedText.remove_prefix(1);
        }
        auto parsed = 0.0F;
        auto const* const begin = std::to_address(parsedText.begin());
        auto const* const end = std::to_address(parsedText.end());
        auto const result = std::from_chars(
            begin,
            end,
            parsed,
            std::chars_format::general
        );
        if (result.ec != std::errc{} || result.ptr != end)
        {
            return invalid(
                std::format(
                    "{} expects a number, got \"{}\"",
                    flag,
                    value
                )
            );
        }
        if (!std::isfinite(parsed))
        {
            return invalid(
                std::format("{} value \"{}\" is not finite", flag, value)
            );
        }
        return parsed;
    }

    [[nodiscard]]
    auto parseRoi(
        std::string_view value,
        std::string_view flag
    ) -> uf::Result<uf::Rect<uf::FrameSpace>>
    {
        auto parts = std::vector<std::string_view>{};
        parts.reserve(4);
        auto remaining = value;
        while (true)
        {
            auto const comma = remaining.find(',');
            if (comma == std::string_view::npos)
            {
                parts.emplace_back(remaining);
                break;
            }
            parts.emplace_back(remaining.substr(0, comma));
            remaining.remove_prefix(comma + 1U);
        }

        if (parts.size() != 4U)
        {
            return invalid(
                std::format(
                    "{} expects 'x,y,width,height', got \"{}\"",
                    flag,
                    value
                )
            );
        }

        UF_TRY_VALUE(x, parseFloat(parts[0], flag));
        UF_TRY_VALUE(y, parseFloat(parts[1], flag));
        UF_TRY_VALUE(width, parseFloat(parts[2], flag));
        UF_TRY_VALUE(height, parseFloat(parts[3], flag));
        return uf::Rect<uf::FrameSpace>{x, y, width, height};
    }

    [[nodiscard]]
    auto parseClickDelay(
        std::string_view value,
        std::string_view flag
    ) -> uf::Result<uf::m0_demo::ClickDelay>
    {
        auto minimum = value;
        auto maximum = value;
        if (auto const dash = value.find('-'); dash != std::string_view::npos)
        {
            minimum = value.substr(0, dash);
            maximum = value.substr(dash + 1U);
        }

        UF_TRY_VALUE(minimumMilliseconds, parseInteger<std::uint64_t>(minimum, flag));
        UF_TRY_VALUE(maximumMilliseconds, parseInteger<std::uint64_t>(maximum, flag));
        return uf::m0_demo::ClickDelay::create(
            minimumMilliseconds,
            maximumMilliseconds
        );
    }

    template <typename Value>
    [[nodiscard]]
    auto require(
        std::optional<Value> value,
        std::string_view flag
    ) -> uf::Result<Value>
    {
        if (!value)
        {
            return invalid(std::format("missing required argument {}", flag));
        }
        return *std::move(value);
    }
}

namespace uf::m0_demo
{
    auto parseArguments(std::span<std::string const> raw) -> Result<Args>
    {
        auto selector = SelectorArgs{};
        auto homeTemplate = std::optional<std::filesystem::path>{};
        auto homeRoi = std::optional<Rect<FrameSpace>>{};
        auto resultTemplate = std::optional<std::filesystem::path>{};
        auto resultRoi = std::optional<Rect<FrameSpace>>{};
        auto resetTemplate = std::optional<std::filesystem::path>{};
        auto resetRoi = std::optional<Rect<FrameSpace>>{};
        auto threshold = std::optional<std::uint64_t>{};
        auto mode = Mode::Guard;
        auto loops = std::uint32_t{1};
        auto maxActionFrameAge = std::chrono::duration_cast<MonotonicInstant::Duration>(
            std::chrono::milliseconds{750}
        );
        auto stallTimeout = std::chrono::duration_cast<MonotonicInstant::Duration>(
            std::chrono::milliseconds{1000}
        );
        auto clickDelay = std::optional<ClickDelay>{};
        auto seed = g_defaultPacingSeed;
        auto log = std::optional<std::filesystem::path>{};

        auto index = std::size_t{0};
        while (index < raw.size())
        {
            auto const& flag = raw[index];
            if (!isValueFlag(flag))
            {
                return invalid(std::format("unknown argument \"{}\"", flag));
            }
            if (index + 1U >= raw.size())
            {
                return invalid(std::format("missing value for {}", flag));
            }
            auto const& value = raw[index + 1U];

            if (isSelectorFlag(flag))
            {
                UF_TRY(parseSelectorValue(selector, flag, value));
            }
            else if (flag == "--home-template")
            {
                homeTemplate = std::filesystem::path{value};
            }
            else if (flag == "--home-roi")
            {
                UF_TRY_VALUE(parsed, parseRoi(value, flag));
                homeRoi = parsed;
            }
            else if (flag == "--result-template")
            {
                resultTemplate = std::filesystem::path{value};
            }
            else if (flag == "--result-roi")
            {
                UF_TRY_VALUE(parsed, parseRoi(value, flag));
                resultRoi = parsed;
            }
            else if (flag == "--reset-template")
            {
                resetTemplate = std::filesystem::path{value};
            }
            else if (flag == "--reset-roi")
            {
                UF_TRY_VALUE(parsed, parseRoi(value, flag));
                resetRoi = parsed;
            }
            else if (flag == "--threshold")
            {
                UF_TRY_VALUE(parsed, parseInteger<std::uint64_t>(value, flag));
                threshold = parsed;
            }
            else if (flag == "--mode")
            {
                UF_TRY_VALUE(parsed, parseMode(value));
                mode = parsed;
            }
            else if (flag == "--loops")
            {
                UF_TRY_VALUE(parsed, parseInteger<std::uint32_t>(value, flag));
                loops = parsed;
            }
            else if (flag == "--max-action-frame-age")
            {
                UF_TRY_VALUE(parsed, parsePositiveDuration(value, flag));
                maxActionFrameAge = parsed;
            }
            else if (flag == "--stall-timeout")
            {
                UF_TRY_VALUE(parsed, parsePositiveDuration(value, flag));
                stallTimeout = parsed;
            }
            else if (flag == "--click-delay-ms")
            {
                UF_TRY_VALUE(parsed, parseClickDelay(value, flag));
                clickDelay = parsed;
            }
            else if (flag == "--seed")
            {
                UF_TRY_VALUE(parsed, parseInteger<std::uint64_t>(value, flag));
                seed = parsed;
            }
            else if (flag == "--log")
            {
                log = std::filesystem::path{value};
            }
            index += 2U;
        }

        if (loops == 0U)
        {
            return invalid("--loops must be at least 1, got 0");
        }

        UF_TRY_VALUE(requiredThreshold, require(std::move(threshold), "--threshold"));
        if (requiredThreshold > maximumAveragePixelSad)
        {
            return invalid(
                std::format(
                    "--threshold must be in 0..={}, got {}",
                    maximumAveragePixelSad,
                    requiredThreshold
                )
            );
        }

        UF_TRY_VALUE(requiredHomeTemplate, require(std::move(homeTemplate), "--home-template"));
        UF_TRY_VALUE(requiredHomeRoi, require(std::move(homeRoi), "--home-roi"));
        UF_TRY_VALUE(
            requiredResultTemplate,
            require(std::move(resultTemplate), "--result-template")
        );
        UF_TRY_VALUE(requiredResultRoi, require(std::move(resultRoi), "--result-roi"));
        UF_TRY_VALUE(
            requiredResetTemplate,
            require(std::move(resetTemplate), "--reset-template")
        );
        UF_TRY_VALUE(requiredResetRoi, require(std::move(resetRoi), "--reset-roi"));

        return Args{
            .m_selector = std::move(selector),
            .m_homeTemplate = std::move(requiredHomeTemplate),
            .m_homeRoi = requiredHomeRoi,
            .m_resultTemplate = std::move(requiredResultTemplate),
            .m_resultRoi = requiredResultRoi,
            .m_resetTemplate = std::move(requiredResetTemplate),
            .m_resetRoi = requiredResetRoi,
            .m_threshold = requiredThreshold,
            .m_mode = mode,
            .m_loops = loops,
            .m_maxActionFrameAge = maxActionFrameAge,
            .m_stallTimeout = stallTimeout,
            .m_clickDelay = clickDelay,
            .m_seed = seed,
            .m_log = std::move(log),
        };
    }

    auto parseCaptureArguments(
        std::span<std::string const> raw
    ) -> Result<CaptureArgs>
    {
        auto selector = SelectorArgs{};
        auto output = std::optional<std::filesystem::path>{};
        auto frames = g_defaultCaptureFrames;
        auto interval = g_defaultCaptureInterval;
        auto log = std::optional<std::filesystem::path>{};

        auto index = std::size_t{0};
        while (index < raw.size())
        {
            auto const& flag = raw[index];
            if (!isCaptureValueFlag(flag))
            {
                return invalid(std::format("unknown capture argument \"{}\"", flag));
            }
            if (index + 1U >= raw.size())
            {
                return invalid(std::format("missing value for {}", flag));
            }
            auto const& value = raw[index + 1U];

            if (isSelectorFlag(flag))
            {
                UF_TRY(parseSelectorValue(selector, flag, value));
            }
            else if (flag == "--out")
            {
                output = std::filesystem::path{value};
            }
            else if (flag == "--frames")
            {
                UF_TRY_VALUE(parsed, parseInteger<std::uint32_t>(value, flag));
                frames = parsed;
            }
            else if (flag == "--interval-ms")
            {
                UF_TRY_VALUE(parsed, parseMilliseconds(value, flag));
                interval = parsed;
            }
            else if (flag == "--log")
            {
                log = std::filesystem::path{value};
            }
            index += 2U;
        }

        if (frames == 0U)
        {
            return invalid("--frames must be at least 1, got 0");
        }

        UF_TRY_VALUE(requiredOutput, require(std::move(output), "--out"));
        if (requiredOutput.empty())
        {
            return invalid("--out must not be empty");
        }

        return CaptureArgs{
            .m_selector = std::move(selector),
            .m_output = std::move(requiredOutput),
            .m_frames = frames,
            .m_interval = interval,
            .m_log = std::move(log),
        };
    }

    auto parseInputAgentArguments(
        std::span<std::string const> raw
    ) -> Result<InputAgentArgs>
    {
        auto windowHandle = std::optional<std::intptr_t>{};
        auto queue = std::optional<std::filesystem::path>{};
        auto results = std::optional<std::filesystem::path>{};
        auto outputDirectory = std::optional<std::filesystem::path>{};
        auto idleTimeout = g_defaultInputAgentIdleTimeout;

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
            .m_windowHandle = requiredWindowHandle,
            .m_queue = std::move(requiredQueue),
            .m_results = std::move(requiredResults),
            .m_outputDirectory = std::move(requiredOutputDirectory),
            .m_idleTimeout = idleTimeout,
        };
    }

    auto usageText() noexcept -> std::string_view
    {
        return
            "Usage: m0-demo [selector] --home-template PATH --home-roi X,Y,W,H "
            "--result-template PATH --result-roi X,Y,W,H --reset-template PATH "
            "--reset-roi X,Y,W,H --threshold 0..255 [options]\n"
            "\n"
            "Selector (optional; an empty selector requires exactly one candidate):\n"
            "  --pid N                      Target process id\n"
            "  --hwnd N|0xHEX               Target window handle\n"
            "  --class TEXT                 Exact window class\n"
            "  --title TEXT                 Exact window title\n"
            "\n"
            "Options:\n"
            "  --mode guard|coexist         Default: guard\n"
            "  --loops N                    Default: 1; must be at least 1\n"
            "  --max-action-frame-age MS    Default: 750\n"
            "  --stall-timeout MS           Default: 1000\n"
            "  --click-delay-ms MS|MIN-MAX  Optional inclusive pre-click delay\n"
            "  --seed N                     Deterministic pacing seed\n"
            "  --log PATH                   Write JSONL to a file instead of stdout\n"
            "  --help                       Show this help without accessing a window\n";
    }

    auto captureUsageText() noexcept -> std::string_view
    {
        return
            "Usage: m0-demo capture [selector] --out PATH [options]\n"
            "\n"
            "Selector (optional; an empty selector requires exactly one candidate):\n"
            "  --pid N                      Target process id\n"
            "  --hwnd N|0xHEX               Target window handle\n"
            "  --title TEXT                 Exact window title\n"
            "\n"
            "Options:\n"
            "  --frames N                   Default: 1; must be at least 1\n"
            "  --interval-ms N              Default: 0; delay between frames\n"
            "  --log PATH                   Write JSONL to a file instead of stdout\n";
    }

    auto inputAgentUsageText() noexcept -> std::string_view
    {
        return
            "Usage: m0-demo input-agent --hwnd N|0xHEX --queue PATH "
            "--results PATH --output-dir DIR [options]\n"
            "\n"
            "Options:\n"
            "  --idle-timeout-s N           Default: 120; must be positive\n";
    }
}
