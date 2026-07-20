#pragma once

#include <compare>
#include <type_traits>
#include <variant>

namespace uf
{
    template <typename Value = std::monostate>
    struct Continue final
    {
        static_assert(!std::is_reference_v<Value>, "Control-flow values must own their state.");

        [[no_unique_address]] Value value{};

        auto operator<=>(Continue const&) const = default;
    };

    template <typename Value = std::monostate>
    struct Break final
    {
        static_assert(!std::is_reference_v<Value>, "Control-flow values must own their state.");

        [[no_unique_address]] Value value{};

        auto operator<=>(Break const&) const = default;
    };

    template <typename BreakValue = std::monostate, typename ContinueValue = std::monostate>
    using ControlFlow = std::variant<Continue<ContinueValue>, Break<BreakValue>>;

    template <typename BreakValue, typename ContinueValue>
    [[nodiscard]]
    constexpr auto isBreak(ControlFlow<BreakValue, ContinueValue> const& flow) noexcept -> bool
    {
        return std::holds_alternative<Break<BreakValue>>(flow);
    }

    template <typename BreakValue, typename ContinueValue>
    [[nodiscard]]
    constexpr auto isContinue(ControlFlow<BreakValue, ContinueValue> const& flow) noexcept -> bool
    {
        return std::holds_alternative<Continue<ContinueValue>>(flow);
    }
}
