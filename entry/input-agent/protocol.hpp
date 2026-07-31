#pragma once

#include <controller/input.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>

#include <chrono>
#include <filesystem>
#include <string_view>
#include <variant>

namespace uf::input_agent
{
    inline constexpr auto k_defaultInputAgentSettle = (
        std::chrono::duration_cast<MonotonicInstant::Duration>(
            std::chrono::milliseconds{400}
        )
    );
    inline constexpr auto k_maximumInputAgentSettle = (
        std::chrono::duration_cast<MonotonicInstant::Duration>(
            std::chrono::milliseconds{5'000}
        )
    );

    struct InputAgentCaptureCommand final
    {
        std::filesystem::path output{};

        auto operator==(InputAgentCaptureCommand const&) const -> bool = default;
    };

    struct InputAgentClickCommand final
    {
        float x{};
        float y{};
        std::filesystem::path outputBefore{};
        std::filesystem::path outputAfter{};
        MonotonicInstant::Duration settle{};

        auto operator==(InputAgentClickCommand const&) const -> bool = default;
    };

    // A keystroke names no position, so this command carries no coordinate. It
    // still frames the action with a before-frame and an after-frame, because
    // the outcome of a key is only observable in the target's own pixels.
    struct InputAgentKeyCommand final
    {
        KeyInput              key;
        std::filesystem::path outputBefore{};
        std::filesystem::path outputAfter{};
        MonotonicInstant::Duration settle{};

        auto operator==(InputAgentKeyCommand const&) const -> bool = default;
    };

    // A wheel notch lands wherever the target hit-tests the pointer, so this
    // command names a coordinate exactly as click does. It carries the same
    // before-frame and after-frame framing too, because a scrolled list is only
    // observable in the target's own pixels.
    struct InputAgentScrollCommand final
    {
        float x{};
        float y{};
        WheelDelta delta;
        std::filesystem::path outputBefore{};
        std::filesystem::path outputAfter{};
        MonotonicInstant::Duration settle{};

        auto operator==(InputAgentScrollCommand const&) const -> bool = default;
    };

    struct InputAgentQuitCommand final
    {
        auto operator==(InputAgentQuitCommand const&) const -> bool = default;
    };

    using InputAgentCommand = std::variant<
        InputAgentCaptureCommand,
        InputAgentClickCommand,
        InputAgentKeyCommand,
        InputAgentScrollCommand,
        InputAgentQuitCommand
    >;

    [[nodiscard]]
    auto parseInputAgentCommand(
        std::string_view line
    ) -> Result<InputAgentCommand>;
}
