#pragma once

#include "cursor.hpp"

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <chrono>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace uf::input_agent
{
    inline constexpr auto k_defaultInputAgentIdleTimeout = (
        std::chrono::duration_cast<MonotonicInstant::Duration>(
            std::chrono::seconds{120}
        )
    );

    struct InputAgentArgs final
    {
        intptr                     windowHandle{};
        std::filesystem::path      queue{};
        std::filesystem::path      results{};
        std::filesystem::path      outputDirectory{};
        MonotonicInstant::Duration idleTimeout{};

        // Consulted only when no cursor exists beside a queue that is already
        // non-empty. Refuse is the zero value, so the unstated policy is the
        // one that asks rather than the one that replays.
        InputAgentQueueStart queueStart{};

        auto operator==(InputAgentArgs const&) const -> bool = default;
    };

    [[nodiscard]]
    auto parseInputAgentArguments(
        std::span<std::string const> raw
    ) -> Result<InputAgentArgs>;

    [[nodiscard]] auto inputAgentUsageText() noexcept -> std::string_view;
}
