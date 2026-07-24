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
    // above so the composition never observes an unset field.
    struct RunArgs final
    {
        std::filesystem::path m_project{};
        std::string           m_selector{};
        std::string           m_page{};
        std::string           m_action{};

        MonotonicInstant::Duration m_timeout{k_defaultRunTimeout};
        MonotonicInstant::Duration m_pollInterval{k_defaultRunPollInterval};
        uint64                     m_budget{k_defaultPixelComparisonBudget};
        MonotonicInstant::Duration m_recognitionTimeout{k_defaultRunRecognitionTimeout};
        MonotonicInstant::Duration m_maxFrameAge{k_defaultRunMaxFrameAge};

        std::filesystem::path m_trace{k_defaultTracePath};

        auto operator==(RunArgs const&) const -> bool = default;
    };

    [[nodiscard]]
    auto parseRunArguments(std::span<std::string const> raw) -> Result<RunArgs>;

    [[nodiscard]] auto runUsageText() noexcept -> std::string_view;
}
