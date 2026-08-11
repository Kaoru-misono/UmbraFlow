#pragma once

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

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

    // The target listing takes no arguments because it discovers the handle
    // required by the privileged exploration entry point.
    [[nodiscard]] auto targetsUsageText() noexcept -> std::string_view;

    [[nodiscard]] auto usageText() -> std::string;
}
