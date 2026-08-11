#pragma once

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <ocr/engine.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace uf::cli
{
    inline constexpr auto k_defaultRecognitionTimeout = (
        std::chrono::duration_cast<MonotonicInstant::Duration>(
            std::chrono::milliseconds{2000}
        )
    );
    inline constexpr auto k_defaultMaxFrameAge = (
        std::chrono::duration_cast<MonotonicInstant::Duration>(
            std::chrono::milliseconds{750}
        )
    );

    // A search's worst case is the number of candidate positions times the
    // template area. This ceiling covers ordinary authored regions while still
    // refusing an accidental full-frame search for a small template.
    inline constexpr auto k_defaultPixelComparisonBudget = uint64{1} << 31;

    inline constexpr auto k_defaultTracePath = std::string_view{
        "umbra-flow-trace.jsonl"
    };

    // How long an exploration session waits with an empty queue before it
    // releases capture and target resources after its agent disappears.
    inline constexpr auto k_defaultExploreIdleTimeout = (
        std::chrono::duration_cast<MonotonicInstant::Duration>(
            std::chrono::seconds{120}
        )
    );

    // The privileged annotation front end. Queue and result paths are required
    // because a durable cursor, rather than a one-shot command, prevents an
    // agent restart from delivering the same input twice.
    struct ExploreArgs final
    {
        std::filesystem::path project{};
        intptr                windowHandle{};
        std::filesystem::path queue{};
        std::filesystem::path results{};

        uint64                     budget{k_defaultPixelComparisonBudget};
        MonotonicInstant::Duration recognitionTimeout{k_defaultRecognitionTimeout};
        MonotonicInstant::Duration maxFrameAge{k_defaultMaxFrameAge};
        MonotonicInstant::Duration idleTimeout{k_defaultExploreIdleTimeout};

        std::filesystem::path trace{k_defaultTracePath};

        std::optional<std::filesystem::path> ocrModels{};

        auto operator==(ExploreArgs const&) const -> bool = default;
    };

    [[nodiscard]]
    auto parseExploreArguments(std::span<std::string const> raw) -> Result<ExploreArgs>;

    [[nodiscard]] auto exploreUsageText() noexcept -> std::string_view;

    // The project directory this binary loads a project out of and registers
    // its deployments' plugins from. One required path and nothing else:
    // everything else a project owns is named by its two root documents rather
    // than by a flag, which is what makes a project data rather than a command
    // line.
    struct OpenArgs final
    {
        std::filesystem::path project{};

        auto operator==(OpenArgs const&) const -> bool = default;
    };

    [[nodiscard]]
    auto parseOpenArguments(std::span<std::string const> raw) -> Result<OpenArgs>;

    [[nodiscard]] auto openUsageText() noexcept -> std::string_view;

    // One PNG already on disk, and how hard to look at it. There is no target
    // and no project: this verb measures a file, which is what makes it usable
    // by a caller that cannot reach a desktop at all.
    //
    // The model directory is required rather than optional as it is for
    // `explore`. platform::bindOcrEngine answers an absent directory with a
    // null engine, and a null engine here would read every image as holding no
    // text -- a fail-open answer indistinguishable from a correct one.
    struct OcrArgs final
    {
        std::filesystem::path image{};
        std::filesystem::path ocrModels{};

        // Absent reads the whole image. Present is the caller stating where the
        // text is, and it is refused when it leaves the image rather than
        // clamped.
        std::optional<PixelRect> rect{};

        ocr::TextLayout layout{ocr::TextLayout::Block};

        std::optional<uint32> maximumLines{};

        auto operator==(OcrArgs const&) const -> bool = default;
    };

    [[nodiscard]]
    auto parseOcrArguments(std::span<std::string const> raw) -> Result<OcrArgs>;

    [[nodiscard]] auto ocrUsageText() noexcept -> std::string_view;

    // The target listing takes no arguments because it discovers the handle
    // required by the privileged exploration entry point.
    [[nodiscard]] auto targetsUsageText() noexcept -> std::string_view;

    [[nodiscard]] auto usageText() -> std::string;
}
