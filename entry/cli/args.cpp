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
        enum class ExploreFlag : uint8
        {
            Project,
            WindowHandle,
            Queue,
            Results,
            Budget,
            RecognitionTimeout,
            MaxFrameAge,
            IdleTimeout,
            Trace,
            OcrModels,
        };

        struct ExploreFlagSpec final
        {
            std::string_view name{};
            ExploreFlag      flag{ExploreFlag::Project};
        };

        constexpr auto k_exploreFlags = std::array{
            ExploreFlagSpec{"--project", ExploreFlag::Project},
            ExploreFlagSpec{"--hwnd", ExploreFlag::WindowHandle},
            ExploreFlagSpec{"--queue", ExploreFlag::Queue},
            ExploreFlagSpec{"--results", ExploreFlag::Results},
            ExploreFlagSpec{"--budget", ExploreFlag::Budget},
            ExploreFlagSpec{
                "--recognition-timeout",
                ExploreFlag::RecognitionTimeout,
            },
            ExploreFlagSpec{"--max-frame-age", ExploreFlag::MaxFrameAge},
            ExploreFlagSpec{"--idle-timeout", ExploreFlag::IdleTimeout},
            ExploreFlagSpec{"--trace", ExploreFlag::Trace},
            ExploreFlagSpec{"--ocr-models", ExploreFlag::OcrModels},
        };

        [[nodiscard]]
        auto findExploreFlag(
            std::string_view name
        ) noexcept -> std::optional<ExploreFlag>
        {
            auto const found = std::ranges::find(
                k_exploreFlags,
                name,
                &ExploreFlagSpec::name
            );
            if (found == k_exploreFlags.end()) return std::nullopt;
            return found->flag;
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
        auto parseWindowHandle(
            std::string_view value,
            std::string_view flag
        ) -> Result<intptr>
        {
            constexpr auto prefix = std::string_view{"0x"};
            auto const digits     = (
                value.starts_with(prefix) ? value.substr(prefix.size())
                                          : std::string_view{}
            );

            auto parsed             = uintptr{};
            auto const* const begin = std::to_address(digits.begin());
            auto const* const end   = std::to_address(digits.end());
            auto const result       = std::from_chars(begin, end, parsed, 16);
            if (digits.empty() || result.ec != std::errc{} || result.ptr != end)
            {
                return invalid(
                    std::format(
                        "{} expects a window handle as 0x-prefixed hexadecimal, "
                        "got \"{}\"; `umbra-flow targets` prints them",
                        flag,
                        value
                    )
                );
            }
            if (parsed == uintptr{0})
            {
                return invalid(
                    std::format("{} expects a window handle, got \"{}\"", flag, value)
                );
            }

            return static_cast<intptr>(parsed);
        }

        template <typename Unit>
        [[nodiscard]]
        auto parseDurationCount(
            uint64 count,
            std::string_view flag
        ) -> Result<MonotonicInstant::Duration>
        {
            using Duration = MonotonicInstant::Duration;
            auto const maximum = std::chrono::duration_cast<Unit>(
                Duration::max()
            );
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

        [[nodiscard]]
        auto requirePath(
            std::optional<std::filesystem::path> value,
            std::string_view flag
        ) -> Result<std::filesystem::path>
        {
            if (!value || value->empty())
            {
                return invalid(std::format("missing required argument {}", flag));
            }
            return *std::move(value);
        }
    }

    auto parseExploreArguments(
        std::span<std::string const> raw
    ) -> Result<ExploreArgs>
    {
        auto project      = std::optional<std::filesystem::path>{};
        auto windowHandle = std::optional<intptr>{};
        auto queue        = std::optional<std::filesystem::path>{};
        auto results      = std::optional<std::filesystem::path>{};

        auto budget             = k_defaultPixelComparisonBudget;
        auto recognitionTimeout = k_defaultRecognitionTimeout;
        auto maxFrameAge        = k_defaultMaxFrameAge;
        auto idleTimeout        = k_defaultExploreIdleTimeout;
        auto trace              = std::filesystem::path{k_defaultTracePath};
        auto ocrModels          = std::optional<std::filesystem::path>{};

        auto index = std::size_t{0};
        while (index < raw.size())
        {
            auto const& name = raw[index];
            auto const flag  = findExploreFlag(name);
            if (!flag)
            {
                return invalid(std::format("unknown argument \"{}\"", name));
            }
            if (index + 1U >= raw.size())
            {
                return invalid(std::format("missing value for {}", name));
            }
            auto const& value = raw[index + 1U];

            switch (*flag)
            {
            case ExploreFlag::Project:
                project = std::filesystem::path{value};
                break;
            case ExploreFlag::WindowHandle:
            {
                UF_TRY_VALUE(parsed, parseWindowHandle(value, name));
                windowHandle = parsed;
                break;
            }
            case ExploreFlag::Queue:
                queue = std::filesystem::path{value};
                break;
            case ExploreFlag::Results:
                results = std::filesystem::path{value};
                break;
            case ExploreFlag::Budget:
            {
                UF_TRY_VALUE(parsed, parseUnsigned(value, name));
                budget = parsed;
                break;
            }
            case ExploreFlag::RecognitionTimeout:
            {
                UF_TRY_VALUE(count, parseUnsigned(value, name));
                UF_TRY_VALUE(
                    parsed,
                    parseDurationCount<std::chrono::milliseconds>(count, name)
                );
                recognitionTimeout = parsed;
                break;
            }
            case ExploreFlag::MaxFrameAge:
            {
                UF_TRY_VALUE(count, parseUnsigned(value, name));
                UF_TRY_VALUE(
                    parsed,
                    parseDurationCount<std::chrono::milliseconds>(count, name)
                );
                maxFrameAge = parsed;
                break;
            }
            case ExploreFlag::IdleTimeout:
            {
                UF_TRY_VALUE(count, parseUnsigned(value, name));
                UF_TRY_VALUE(
                    parsed,
                    parseDurationCount<std::chrono::seconds>(count, name)
                );
                idleTimeout = parsed;
                break;
            }
            case ExploreFlag::Trace:
                trace = std::filesystem::path{value};
                break;
            case ExploreFlag::OcrModels:
                ocrModels = std::filesystem::path{value};
                break;
            }
            index += 2U;
        }

        UF_TRY_VALUE(requiredProject, requirePath(std::move(project), "--project"));
        UF_TRY_VALUE(requiredQueue, requirePath(std::move(queue), "--queue"));
        UF_TRY_VALUE(requiredResults, requirePath(std::move(results), "--results"));
        if (!windowHandle)
        {
            return invalid("missing required argument --hwnd");
        }

        return ExploreArgs{
            .project            = std::move(requiredProject),
            .windowHandle       = *windowHandle,
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

    auto exploreUsageText() noexcept -> std::string_view
    {
        return
            "Usage:\n"
            "  umbra-flow explore --project DIR --hwnd 0xHANDLE "
            "--queue PATH --results PATH [options]\n"
            "\n"
            "Executes privileged annotation chunks arriving as JSON lines in\n"
            "--queue and writes one durable result line per chunk. This entry\n"
            "does not execute project business tasks.\n"
            "\n"
            "Required:\n"
            "  --project DIR                Annotation project directory\n"
            "  --hwnd 0xHANDLE              Target handle from `umbra-flow targets`\n"
            "  --queue PATH                 Chunk queue this session tails\n"
            "  --results PATH               Durable result lines\n"
            "\n"
            "Options:\n"
            "  --budget N                   Pixel comparison ceiling per recognition\n"
            "  --recognition-timeout MS     Per-recognition deadline; default: 2000\n"
            "  --max-frame-age MS           Action frame age ceiling; default: 750\n"
            "  --idle-timeout S             End after an idle queue; default: 120\n"
            "  --trace PATH                 Trace JSONL path; default: "
            "umbra-flow-trace.jsonl\n"
            "  --ocr-models DIR             OCR model directory; default: disabled\n";
    }

    auto targetsUsageText() noexcept -> std::string_view
    {
        return
            "Usage:\n"
            "  umbra-flow targets\n"
            "\n"
            "Prints one line per capturable desktop window. Each line contains\n"
            "the handle, class, client size, DPI, minimized state, and title.\n"
            "The handle is the value accepted by explore --hwnd.\n"
            "\n"
            "This command takes no arguments.\n";
    }

    auto usageText() -> std::string
    {
        auto text = std::string{exploreUsageText()};
        text += '\n';
        text += targetsUsageText();
        return text;
    }
}
