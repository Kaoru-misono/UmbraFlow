#pragma once

#include <type_traits>
#include <utility>
#include <variant>

namespace umbra_flow
{
    template <typename... Handlers>
    struct Overload : Handlers...
    {
        using Handlers::operator()...;
    };

    template <typename... Handlers>
    Overload(Handlers...) -> Overload<Handlers...>;

    template <typename Variant, typename... Handlers>
    constexpr decltype(auto) matchVariant(Variant&& variant, Handlers&&... handlers)
    {
        using Visitor = Overload<std::remove_cvref_t<Handlers>...>;
        return std::visit(
            Visitor{std::forward<Handlers>(handlers)...},
            std::forward<Variant>(variant)
        );
    }
}
