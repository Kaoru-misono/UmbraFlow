#pragma once

#include <concepts>
#include <limits>
#include <optional>
#include <type_traits>

namespace umbra_flow
{
    template <typename Value>
    concept CheckedInteger = (
        std::integral<Value>
        && !std::same_as<std::remove_cv_t<Value>, bool>
        && !std::same_as<std::remove_cv_t<Value>, char>
        && !std::same_as<std::remove_cv_t<Value>, wchar_t>
        && !std::same_as<std::remove_cv_t<Value>, char8_t>
        && !std::same_as<std::remove_cv_t<Value>, char16_t>
        && !std::same_as<std::remove_cv_t<Value>, char32_t>
    );

    template <CheckedInteger Value>
    [[nodiscard]]
    constexpr auto checkedAdd(Value left, Value right) noexcept -> std::optional<Value>
    {
        auto constexpr maximum = std::numeric_limits<Value>::max();

        if constexpr (std::unsigned_integral<Value>)
        {
            if (right > maximum - left)
            {
                return std::nullopt;
            }
        }
        else
        {
            auto constexpr minimum = std::numeric_limits<Value>::min();
            if (
                (right > 0 && left > maximum - right)
                || (right < 0 && left < minimum - right)
            )
            {
                return std::nullopt;
            }
        }

        return static_cast<Value>(left + right);
    }

    template <CheckedInteger Value>
    [[nodiscard]]
    constexpr auto checkedSubtract(Value left, Value right) noexcept -> std::optional<Value>
    {
        if constexpr (std::unsigned_integral<Value>)
        {
            if (left < right)
            {
                return std::nullopt;
            }
        }
        else
        {
            auto constexpr minimum = std::numeric_limits<Value>::min();
            auto constexpr maximum = std::numeric_limits<Value>::max();
            if (
                (right > 0 && left < minimum + right)
                || (right < 0 && left > maximum + right)
            )
            {
                return std::nullopt;
            }
        }

        return static_cast<Value>(left - right);
    }

    namespace detail
    {
        template <CheckedInteger Value>
        [[nodiscard]]
        constexpr auto unsignedMagnitude(Value value) noexcept -> std::make_unsigned_t<Value>
        {
            using Unsigned = std::make_unsigned_t<Value>;
            auto const converted = static_cast<Unsigned>(value);

            if constexpr (std::signed_integral<Value>)
            {
                if (value < 0)
                {
                    return static_cast<Unsigned>(Unsigned{0} - converted);
                }
            }

            return converted;
        }
    }

    template <CheckedInteger Value>
    [[nodiscard]]
    constexpr auto checkedMultiply(Value left, Value right) noexcept -> std::optional<Value>
    {
        using Unsigned = std::make_unsigned_t<Value>;

        if constexpr (std::unsigned_integral<Value>)
        {
            if (right != 0 && left > std::numeric_limits<Value>::max() / right)
            {
                return std::nullopt;
            }

            return static_cast<Value>(left * right);
        }
        else
        {
            auto const leftMagnitude = detail::unsignedMagnitude(left);
            auto const rightMagnitude = detail::unsignedMagnitude(right);
            auto const negative = (left < 0) != (right < 0);
            auto const negativeLimit = detail::unsignedMagnitude(std::numeric_limits<Value>::min());
            auto const positiveLimit = static_cast<Unsigned>(std::numeric_limits<Value>::max());
            auto const limit = negative ? negativeLimit : positiveLimit;

            if (rightMagnitude != 0 && leftMagnitude > limit / rightMagnitude)
            {
                return std::nullopt;
            }

            auto const magnitude = static_cast<Unsigned>(leftMagnitude * rightMagnitude);
            if (!negative)
            {
                return static_cast<Value>(magnitude);
            }

            if (magnitude == negativeLimit)
            {
                return std::numeric_limits<Value>::min();
            }

            return static_cast<Value>(-static_cast<Value>(magnitude));
        }
    }

    template <CheckedInteger Value>
    [[nodiscard]]
    constexpr auto checkedDivide(Value left, Value right) noexcept -> std::optional<Value>
    {
        if (right == Value{0})
        {
            return std::nullopt;
        }

        if constexpr (std::signed_integral<Value>)
        {
            if (
                left == std::numeric_limits<Value>::min()
                && right == Value{-1}
            )
            {
                return std::nullopt;
            }
        }

        return static_cast<Value>(left / right);
    }

    template <CheckedInteger Value>
    [[nodiscard]]
    constexpr auto checkedRemainder(Value left, Value right) noexcept -> std::optional<Value>
    {
        if (right == Value{0})
        {
            return std::nullopt;
        }

        if constexpr (std::signed_integral<Value>)
        {
            if (
                left == std::numeric_limits<Value>::min()
                && right == Value{-1}
            )
            {
                return std::nullopt;
            }
        }

        return static_cast<Value>(left % right);
    }
}
