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
    inline constexpr auto k_defaultRunTimeout = (
        std::chrono::duration_cast<MonotonicInstant::Duration>(
            std::chrono::seconds{30}
        )
    );
    inline constexpr auto k_defaultRunPollInterval = (
        std::chrono::duration_cast<MonotonicInstant::Duration>(
            std::chrono::milliseconds{250}
        )
    );
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

    // A generous per-recognition ceiling on template pixel comparisons: high
    // enough that a legitimate full-frame search never exhausts it, low enough
    // that a pathological project cannot spin without bound before failing closed.
    inline constexpr auto k_defaultPixelComparisonBudget = uint64{1} << 28;

    inline constexpr auto k_defaultTracePath = std::string_view{
        "umbra-flow-trace.jsonl"
    };

    // Parsed inputs for the `run` subcommand. Every field carries a resolved
    // value once parsing succeeds; the optional flags fall back to the defaults
    // above so the composition never observes an unset field. A run is always a
    // project-owned task addressed as (project, task): the single-step
    // --page/--action smoke path is gone, so there is one mode and nothing to
    // choose between.
    struct RunArgs final
    {
        std::filesystem::path project{};
        std::string           selector{};
        std::string           task{};

        MonotonicInstant::Duration timeout{k_defaultRunTimeout};
        MonotonicInstant::Duration pollInterval{k_defaultRunPollInterval};
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
