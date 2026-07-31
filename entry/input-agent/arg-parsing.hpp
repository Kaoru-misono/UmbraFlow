#pragma once

#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <charconv>
#include <concepts>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

// The command-line primitives the input agent and the frozen M0 demo both
// spell. They were one anonymous namespace until the two programs split, and
// they are exposed here rather than duplicated because a window handle that
// parsed in one program and not the other would be a difference nobody
// intended. They are implementation detail of both parsers, not vocabulary
// either program offers its callers.
namespace uf::input_agent::detail
{
    [[nodiscard]]
    inline auto invalid(std::string message) -> std::unexpected<Error>
    {
        return fail(
            AutomationErrorKind::InvalidResource,
            std::move(message)
        );
    }

    template <std::integral Value>
    [[nodiscard]]
    auto parseInteger(
        std::string_view value,
        std::string_view flag,
        int base = 10
    ) -> Result<Value>
    {
        auto const supplied = value;
        if (value.starts_with('+'))
        {
            value.remove_prefix(1);
        }
        auto parsed = Value{};
        auto const* const begin = std::to_address(value.begin());
        auto const* const end = std::to_address(value.end());
        auto const result = std::from_chars(
            begin,
            end,
            parsed,
            base
        );
        if (result.ec != std::errc{} || result.ptr != end)
        {
            return invalid(
                std::format(
                    "{} expects an integer, got \"{}\"",
                    flag,
                    supplied
                )
            );
        }

        return parsed;
    }

    [[nodiscard]]
    inline auto parseWindowHandle(
        std::string_view value,
        std::string_view flag
    ) -> Result<intptr>
    {
        auto base = 10;
        if (value.starts_with("0x") || value.starts_with("0X"))
        {
            value.remove_prefix(2);
            base = 16;
        }

        UF_TRY_VALUE(parsed, parseInteger<int64>(value, flag, base));
        auto const converted = checkedCast<intptr>(parsed);
        if (!converted)
        {
            return invalid(
                std::format(
                    "{} window handle is outside the machine-word range",
                    flag
                )
            );
        }
        return *converted;
    }

    template <typename Value>
    [[nodiscard]]
    auto require(
        std::optional<Value> value,
        std::string_view flag
    ) -> Result<Value>
    {
        if (!value)
        {
            return invalid(std::format("missing required argument {}", flag));
        }
        return *std::move(value);
    }
}
