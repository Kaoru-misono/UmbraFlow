#include "args.hpp"

#include <core/numeric/checked-cast.hpp>
#include <core/safety/checked-access.hpp>
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

        constexpr auto k_openProjectFlag = std::string_view{"--project"};

        constexpr auto k_reclaimRuntimeFlag = std::string_view{"--runtime"};

        enum class ObserveFlag : uint8
        {
            Project,
            WindowHandle,
            Runtime,
            OcrModels,
            Budget,
            RecognitionTimeout,
            Trace,
        };

        struct ObserveFlagSpec final
        {
            std::string_view name{};
            ObserveFlag      flag{ObserveFlag::Project};
        };

        constexpr auto k_observeFlags = std::array{
            ObserveFlagSpec{"--project", ObserveFlag::Project},
            ObserveFlagSpec{"--hwnd", ObserveFlag::WindowHandle},
            ObserveFlagSpec{"--runtime", ObserveFlag::Runtime},
            ObserveFlagSpec{"--ocr-models", ObserveFlag::OcrModels},
            ObserveFlagSpec{"--budget", ObserveFlag::Budget},
            ObserveFlagSpec{
                "--recognition-timeout",
                ObserveFlag::RecognitionTimeout,
            },
            ObserveFlagSpec{"--trace", ObserveFlag::Trace},
        };

        [[nodiscard]]
        auto findObserveFlag(
            std::string_view name
        ) noexcept -> std::optional<ObserveFlag>
        {
            auto const found = std::ranges::find(
                k_observeFlags,
                name,
                &ObserveFlagSpec::name
            );
            if (found == k_observeFlags.end()) return std::nullopt;
            return found->flag;
        }

        enum class OcrFlag : uint8
        {
            Image,
            OcrModels,
            Rect,
            Layout,
            MaximumLines,
        };

        struct OcrFlagSpec final
        {
            std::string_view name{};
            OcrFlag          flag{OcrFlag::Image};
        };

        constexpr auto k_ocrFlags = std::array{
            OcrFlagSpec{"--image", OcrFlag::Image},
            OcrFlagSpec{"--ocr-models", OcrFlag::OcrModels},
            OcrFlagSpec{"--rect", OcrFlag::Rect},
            OcrFlagSpec{"--layout", OcrFlag::Layout},
            OcrFlagSpec{"--max-lines", OcrFlag::MaximumLines},
        };

        [[nodiscard]]
        auto findOcrFlag(std::string_view name) noexcept -> std::optional<OcrFlag>
        {
            auto const found = std::ranges::find(
                k_ocrFlags,
                name,
                &OcrFlagSpec::name
            );
            if (found == k_ocrFlags.end()) return std::nullopt;
            return found->flag;
        }

        struct OcrLayoutSpec final
        {
            std::string_view name{};
            ocr::TextLayout  layout{ocr::TextLayout::Block};
        };

        // The whole vocabulary of --layout, and the only place it is spelled.
        constexpr auto k_ocrLayouts = std::array{
            OcrLayoutSpec{"block", ocr::TextLayout::Block},
            OcrLayoutSpec{"single-line", ocr::TextLayout::SingleLine},
        };

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

        [[nodiscard]]
        auto parseUnsigned32(
            std::string_view value,
            std::string_view flag
        ) -> Result<uint32>
        {
            UF_TRY_VALUE(parsed, parseUnsigned(value, flag));
            auto const narrowed = checkedCast<uint32>(parsed);
            if (!narrowed)
            {
                return invalid(
                    std::format("{} value {} does not fit in 32 bits", flag, parsed)
                );
            }
            return *narrowed;
        }

        [[nodiscard]]
        auto parseLayout(
            std::string_view value,
            std::string_view flag
        ) -> Result<ocr::TextLayout>
        {
            auto const found = std::ranges::find(
                k_ocrLayouts,
                value,
                &OcrLayoutSpec::name
            );
            if (found == k_ocrLayouts.end())
            {
                return invalid(
                    std::format(
                        "{} expects block or single-line, got \"{}\"",
                        flag,
                        value
                    )
                );
            }
            return found->layout;
        }

        // x,y,width,height in the image's own pixels. Four components exactly:
        // a caller that wrote three has not described a rectangle, and
        // defaulting the missing one would read a region nobody asked for.
        [[nodiscard]]
        auto parseRect(
            std::string_view value,
            std::string_view flag
        ) -> Result<PixelRect>
        {
            auto fields    = std::array<uint32, 4>{};
            auto remaining = value;
            for (auto index = std::size_t{0}; index < fields.size(); ++index)
            {
                auto const last      = index + 1U == fields.size();
                auto const separator = remaining.find(',');
                if (last != (separator == std::string_view::npos))
                {
                    return invalid(
                        std::format(
                            "{} expects x,y,width,height, got \"{}\"",
                            flag,
                            value
                        )
                    );
                }

                UF_TRY_VALUE(
                    parsed,
                    parseUnsigned32(remaining.substr(0U, separator), flag)
                );
                checkedAt(fields, index) = parsed;
                if (!last)
                {
                    remaining = remaining.substr(separator + 1U);
                }
            }

            UF_TRY_VALUE_CONTEXT(
                rect,
                PixelRect::create(fields[0], fields[1], fields[2], fields[3]),
                std::string{flag}
            );
            return rect;
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

    auto parseOpenArguments(std::span<std::string const> raw) -> Result<OpenArgs>
    {
        auto project = std::optional<std::filesystem::path>{};

        auto index = std::size_t{0};
        while (index < raw.size())
        {
            auto const& name = raw[index];
            if (name != k_openProjectFlag)
            {
                return invalid(std::format("unknown argument \"{}\"", name));
            }
            if (index + 1U >= raw.size())
            {
                return invalid(std::format("missing value for {}", name));
            }
            project = std::filesystem::path{raw[index + 1U]};
            index += 2U;
        }

        UF_TRY_VALUE(
            requiredProject,
            requirePath(std::move(project), k_openProjectFlag)
        );
        return OpenArgs{.project = std::move(requiredProject)};
    }

    auto parseReclaimArguments(
        std::span<std::string const> raw
    ) -> Result<ReclaimArgs>
    {
        auto runtime = std::optional<std::filesystem::path>{};

        auto index = std::size_t{0};
        while (index < raw.size())
        {
            auto const& name = raw[index];
            if (name != k_reclaimRuntimeFlag)
            {
                return invalid(std::format("unknown argument \"{}\"", name));
            }
            if (index + 1U >= raw.size())
            {
                return invalid(std::format("missing value for {}", name));
            }
            runtime = std::filesystem::path{raw[index + 1U]};
            index += 2U;
        }

        UF_TRY_VALUE(
            requiredRuntime,
            requirePath(std::move(runtime), k_reclaimRuntimeFlag)
        );
        return ReclaimArgs{.runtime = std::move(requiredRuntime)};
    }

    auto parseObserveArguments(
        std::span<std::string const> raw
    ) -> Result<ObserveArgs>
    {
        auto project      = std::optional<std::filesystem::path>{};
        auto windowHandle = std::optional<intptr>{};
        auto runtime      = std::optional<std::filesystem::path>{};
        auto ocrModels    = std::optional<std::filesystem::path>{};

        auto budget             = k_defaultPixelComparisonBudget;
        auto recognitionTimeout = k_defaultRecognitionTimeout;
        auto trace              = std::filesystem::path{k_defaultObserveTracePath};

        auto index = std::size_t{0};
        while (index < raw.size())
        {
            auto const& name = raw[index];
            auto const flag  = findObserveFlag(name);
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
            case ObserveFlag::Project:
                project = std::filesystem::path{value};
                break;
            case ObserveFlag::WindowHandle:
            {
                UF_TRY_VALUE(parsed, parseWindowHandle(value, name));
                windowHandle = parsed;
                break;
            }
            case ObserveFlag::Runtime:
                runtime = std::filesystem::path{value};
                break;
            case ObserveFlag::OcrModels:
                ocrModels = std::filesystem::path{value};
                break;
            case ObserveFlag::Budget:
            {
                UF_TRY_VALUE(parsed, parseUnsigned(value, name));
                budget = parsed;
                break;
            }
            case ObserveFlag::RecognitionTimeout:
            {
                UF_TRY_VALUE(count, parseUnsigned(value, name));
                UF_TRY_VALUE(
                    parsed,
                    parseDurationCount<std::chrono::milliseconds>(count, name)
                );
                recognitionTimeout = parsed;
                break;
            }
            case ObserveFlag::Trace:
                trace = std::filesystem::path{value};
                break;
            }
            index += 2U;
        }

        UF_TRY_VALUE(requiredProject, requirePath(std::move(project), "--project"));
        UF_TRY_VALUE(requiredRuntime, requirePath(std::move(runtime), "--runtime"));
        UF_TRY_VALUE(
            requiredModels,
            requirePath(std::move(ocrModels), "--ocr-models")
        );
        if (!windowHandle)
        {
            return invalid("missing required argument --hwnd");
        }

        return ObserveArgs{
            .project            = std::move(requiredProject),
            .windowHandle       = *windowHandle,
            .runtime            = std::move(requiredRuntime),
            .ocrModels          = std::move(requiredModels),
            .budget             = budget,
            .recognitionTimeout = recognitionTimeout,
            .trace              = std::move(trace),
        };
    }

    auto parseOcrArguments(std::span<std::string const> raw) -> Result<OcrArgs>
    {
        auto image     = std::optional<std::filesystem::path>{};
        auto ocrModels = std::optional<std::filesystem::path>{};

        auto rect         = std::optional<PixelRect>{};
        auto layout       = ocr::TextLayout::Block;
        auto maximumLines = std::optional<uint32>{};

        auto index = std::size_t{0};
        while (index < raw.size())
        {
            auto const& name = raw[index];
            auto const flag  = findOcrFlag(name);
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
            case OcrFlag::Image:
                image = std::filesystem::path{value};
                break;
            case OcrFlag::OcrModels:
                ocrModels = std::filesystem::path{value};
                break;
            case OcrFlag::Rect:
            {
                UF_TRY_VALUE(parsed, parseRect(value, name));
                rect = parsed;
                break;
            }
            case OcrFlag::Layout:
            {
                UF_TRY_VALUE(parsed, parseLayout(value, name));
                layout = parsed;
                break;
            }
            case OcrFlag::MaximumLines:
            {
                UF_TRY_VALUE(parsed, parseUnsigned32(value, name));
                maximumLines = parsed;
                break;
            }
            }
            index += 2U;
        }

        UF_TRY_VALUE(requiredImage, requirePath(std::move(image), "--image"));
        UF_TRY_VALUE(
            requiredModels,
            requirePath(std::move(ocrModels), "--ocr-models")
        );

        return OcrArgs{
            .image        = std::move(requiredImage),
            .ocrModels    = std::move(requiredModels),
            .rect         = rect,
            .layout       = layout,
            .maximumLines = maximumLines,
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

    auto openUsageText() noexcept -> std::string_view
    {
        return
            "Usage:\n"
            "  umbra-flow open --project DIR\n"
            "\n"
            "Loads the project directory at DIR, verifies the RuntimeArtifact\n"
            "it names the way the Operator's installer verifies it, registers\n"
            "every deployment's plugin with the Operator, and prints what it\n"
            "found. It reaches no target and runs no plan: what it answers is\n"
            "whether this binary accepts the directory as a project.\n"
            "\n"
            "The artifact report names the root hash the artifact was verified\n"
            "against and the two schema digests this binary accepted, so a\n"
            "reader can tell a verified artifact from an unread one.\n"
            "\n"
            "Required:\n"
            "  --project DIR                Project directory holding\n"
            "                               umbraflow-project.json and\n"
            "                               umbraflow-conformance.json\n"
            "\n"
            "Exits non-zero when the directory or its RuntimeArtifact is\n"
            "refused, and when both load but a deployment's plugin does not\n"
            "register.\n";
    }

    auto ocrUsageText() noexcept -> std::string_view
    {
        return
            "Usage:\n"
            "  umbra-flow ocr --image FILE --ocr-models DIR [options]\n"
            "\n"
            "Reads a PNG already on disk and prints one JSON object on stdout:\n"
            "the image extent, and every line found, each with its text, its\n"
            "rectangle in image pixels and its confidence in basis points. The\n"
            "output is RFC 8785 canonical, so two runs over the same pixels\n"
            "produce the same bytes and a caller may hash or diff it.\n"
            "\n"
            "It reaches no target and opens no capture. Measuring rectangles is\n"
            "what a caller reading a screenshot cannot do for itself; naming\n"
            "what one means is what it can.\n"
            "\n"
            "Required:\n"
            "  --image FILE                 PNG to read\n"
            "  --ocr-models DIR             Directory holding\n"
            "                               ppocr-v6-small-rec and\n"
            "                               ppocr-v6-small-det\n"
            "\n"
            "Options:\n"
            "  --rect X,Y,W,H               Region to read; default: all of it\n"
            "  --layout block|single-line   Locate every line, or read the rect\n"
            "                               as exactly one; default: block\n"
            "  --max-lines N                Refuse, rather than truncate, past\n"
            "                               N lines\n";
    }

    auto observeUsageText() noexcept -> std::string_view
    {
        return
            "Usage:\n"
            "  umbra-flow observe --project DIR --hwnd 0xHANDLE --runtime DIR\n"
            "                     --ocr-models DIR "
            "[options]\n"
            "\n"
            "Runs the production path once, read only. It loads the project at\n"
            "--project, opens the RuntimeArtifact that project names from the\n"
            "Operator production root at --runtime, activates it, binds the\n"
            "window --hwnd names, takes ONE observation, and prints the\n"
            "StateResolution the trusted resolver produced.\n"
            "\n"
            "It stops there. It proposes no plan, mints no Receipt and\n"
            "delivers nothing to the target; the action sink it builds exists\n"
            "so the engine has one and posts nothing.\n"
            "\n"
            "It writes nothing at --runtime. The Operator database is opened\n"
            "read-only, so no byte of it changes: no directory is created, no\n"
            "session epoch advances, no control lease or session is cleared,\n"
            "no in-flight dispatch is resolved, and no CAS object is\n"
            "published. Running this twice leaves the Operator exactly as\n"
            "running it once does, and running it against a --runtime that\n"
            "does not already hold an installed generation is refused rather\n"
            "than bootstrapped.\n"
            "\n"
            "The printed document carries the resolution kind, the ordered\n"
            "surface stack, and one entry per Reader every reporting Binding\n"
            "named -- each with its ui_target, its reader, and either the text\n"
            "it read or the reason it could not. Those bytes are exactly what\n"
            "a project's plugin would be handed as ui_snapshot.\n"
            "\n"
            "Required:\n"
            "  --project DIR                Project directory holding\n"
            "                               umbraflow-project.json\n"
            "  --hwnd 0xHANDLE              Target handle from `umbra-flow targets`\n"
            "  --runtime DIR                Operator production root holding the\n"
            "                               installed RuntimeArtifact\n"
            "  --ocr-models DIR             Directory holding\n"
            "                               ppocr-v6-small-rec and\n"
            "                               ppocr-v6-small-det\n"
            "\n"
            "Options:\n"
            "  --budget N                   Pixel comparison ceiling per search\n"
            "  --recognition-timeout MS     Per-recognition deadline; default: 2000\n"
            "  --trace PATH                 Trace JSONL path; default: "
            "umbra-flow-observe-trace.jsonl\n";
    }

    auto reclaimUsageText() noexcept -> std::string_view
    {
        return
            "Usage:\n"
            "  umbra-flow reclaim --runtime DIR\n"
            "\n"
            "Removes every RuntimeArtifact directory the Operator root at\n"
            "--runtime no longer references, and every staging tree an\n"
            "interrupted publication left behind. It prints how many of each it\n"
            "removed.\n"
            "\n"
            "An artifact directory is unreferenced when no installed generation\n"
            "and no active pin names it. A failed publication is the ordinary\n"
            "way one appears: the bytes are content addressed, so the failing\n"
            "publisher may not delete them -- a concurrent publisher may have\n"
            "put the identical bytes there -- and only a pass that reads the\n"
            "whole reference set at once may decide. This verb is that pass, and\n"
            "it is the only way to run it.\n"
            "\n"
            "It refuses while anything else holds the root, because the Operator\n"
            "admits one owner at a time. Run it when no session is running.\n"
            "\n"
            "Required:\n"
            "  --runtime DIR                Operator production root to sweep\n"
            "\n"
            "Exits non-zero when the root cannot be opened or the sweep cannot\n"
            "finish. Reclaiming nothing is a success, and reports zero.\n";
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
        text += observeUsageText();
        text += '\n';
        text += ocrUsageText();
        text += '\n';
        text += openUsageText();
        text += '\n';
        text += reclaimUsageText();
        text += '\n';
        text += targetsUsageText();
        return text;
    }
}
