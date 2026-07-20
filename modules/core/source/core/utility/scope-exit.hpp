#pragma once

#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

namespace uf
{
    template <typename Function>
    concept ScopeExitFunction = (
        std::is_nothrow_move_constructible_v<Function>
        && std::is_nothrow_invocable_v<Function&>
    );

    template <ScopeExitFunction Function>
    class ScopeExit final
    {
        Function m_function;
        bool m_active{true};

    public:
        explicit ScopeExit(Function&& function) noexcept
            : m_function{std::move(function)}
        {
        }

        ScopeExit(ScopeExit const&) = delete;
        auto operator=(ScopeExit const&) -> ScopeExit& = delete;

        ScopeExit(ScopeExit&& other) noexcept
            : m_function{std::move(other.m_function)}
            , m_active{std::exchange(other.m_active, false)}
        {
        }

        auto operator=(ScopeExit&&) -> ScopeExit& = delete;

        ~ScopeExit() noexcept
        {
            if (m_active)
            {
                std::invoke(m_function);
            }
        }

        auto release() noexcept -> void { m_active = false; }
    };

    template <typename Function>
    ScopeExit(Function&&) -> ScopeExit<std::remove_cvref_t<Function>>;

    template <typename Function>
        requires (
            !std::is_lvalue_reference_v<Function>
            && ScopeExitFunction<std::remove_cvref_t<Function>>
        )
    [[nodiscard]]
    auto scopeExit(Function&& function) noexcept -> ScopeExit<std::remove_cvref_t<Function>>
    {
        return ScopeExit<std::remove_cvref_t<Function>>{std::forward<Function>(function)};
    }
}
