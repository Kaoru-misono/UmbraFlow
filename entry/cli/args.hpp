#pragma once

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <chrono>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace uf::cli
{
    inline constexpr auto k_defaultRunRecognitionTimeout = (
        std::chrono::duration_cast<MonotonicInstant::Duration>(
            std::chrono::milliseconds{2000}
        )
    );
    inline constexpr auto k_defaultRunMaxFrameAge = (
        std::chrono::duration_cast<MonotonicInstant::Duration>(
            std::chrono::milliseconds{750}
        )
    );

    // A per-recognition ceiling on template pixel comparisons, shared by
    // `umbra-flow run` and `umbra-authoring` so one number governs both.
    //
    // What a search costs is arithmetic rather than a guess. One candidate
    // position costs templateWidth * templateHeight comparisons, and the search
    // walks (roiWidth - templateWidth + 1) * (roiHeight - templateHeight + 1)
    // positions, so its worst case is the product; pruning only ever lowers it.
    // The figure to cover is not one search, because evaluatePage shares one
    // budget across every page anchor in the catalog -- it is the sum over them.
    //
    // The envelope: a page evaluation whose anchors are each as costly as the
    // widest ordinary authored one, a 200x50 template in a 480x90 search region.
    // That is 281 * 41 = 11,521 positions x 10,000 pixels = 115,210,000
    // comparisons, and eight pages identified by two anchors each make sixteen
    // of them: 1,843,360,000. 2^31 = 2,147,483,648 covers that with headroom.
    //
    // It deliberately does not cover a full-frame search for a small template.
    // A 66x46 template over 1600x900 walks 1535 * 855 = 1,312,425 positions x
    // 3,036 pixels = 3,984,522,300 comparisons, about 1.9x this ceiling, so such
    // a project has to author a search region or raise --budget on purpose.
    // Sizing the default for that case instead would leave nothing failing
    // closed on a project whose geometry is genuinely unbounded.
    inline constexpr auto k_defaultPixelComparisonBudget = uint64{1} << 31;

    inline constexpr auto k_defaultTracePath = std::string_view{
        "umbra-flow-trace.jsonl"
    };

    // Parsed inputs for the `run` subcommand. Every field carries a resolved
    // value once parsing succeeds; the optional flags fall back to the defaults
    // above so the composition never observes an unset field. A run is always a
    // project-owned task addressed as (project, task): the single-step
    // --page/--action smoke path is gone, so there is one mode and nothing to
    // choose between.
    //
    // How long a task waits for a page, and how often it re-observes while
    // waiting, are deliberately absent. The wait loop is the framework's Luau,
    // so those two numbers are written in the task that waits -- a CLI flag for
    // them would be an operator overriding a decision the script owns, and
    // after the loop moved there was nothing left for such a flag to reach.
    struct RunArgs final
    {
        std::filesystem::path project{};
        std::string           selector{};
        std::string           task{};

        uint64                     budget{k_defaultPixelComparisonBudget};
        MonotonicInstant::Duration recognitionTimeout{k_defaultRunRecognitionTimeout};
        MonotonicInstant::Duration maxFrameAge{k_defaultRunMaxFrameAge};

        std::filesystem::path trace{k_defaultTracePath};

        auto operator==(RunArgs const&) const -> bool = default;
    };

    [[nodiscard]]
    auto parseRunArguments(std::span<std::string const> raw) -> Result<RunArgs>;

    [[nodiscard]] auto runUsageText() noexcept -> std::string_view;
}
