#pragma once

#include <controller/input.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <domain/space.hpp>

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

    // Reading one line of text off the target, and the only verb that answers
    // with what the screen SAYS rather than with a file to go and look at.
    //
    // It names no output path, and that absence is the point: measuring text
    // during an annotation session otherwise means capturing a PNG and reading
    // it with human eyes, and the verb exists to remove that step. Nothing is
    // delivered either, so this carries neither the before/after framing nor the
    // settle the three input verbs need.
    //
    // The rectangle is in the captured frame's OWN PIXEL SPACE -- what an author
    // measures on a capture PNG -- rather than the client space `click` and
    // `scroll` take. `capture` reports both sizes and the delta between them,
    // which is what tells an author whether the two differ on this target.
    struct InputAgentReadCommand final
    {
        // No in-class initializer: PixelRect has no default state, and a rect
        // this layer invented would be one the operator never asked for.
        PixelRect rect;

        auto operator==(InputAgentReadCommand const&) const -> bool = default;
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
        InputAgentReadCommand,
        InputAgentQuitCommand
    >;

    [[nodiscard]]
    auto parseInputAgentCommand(
        std::string_view line
    ) -> Result<InputAgentCommand>;
}
