#pragma once

#include <core/error/result.hpp>

#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

namespace uf::m0_demo
{
    [[nodiscard]] auto installConsoleControlHandler() -> Status;
    [[nodiscard]] auto stopRequested() noexcept -> bool;

    template <
        typename State,
        typename Release,
        typename Close,
        typename Finalize,
        typename Flush
    >
    [[nodiscard]]
    auto runShutdown(
        State& state,
        Release&& release,
        Close&& close,
        Finalize&& finalize,
        Flush&& flush
    ) -> std::invoke_result_t<Release, State&>
    {
        using ResultType = std::invoke_result_t<Release, State&>;
        static_assert(std::same_as<ResultType, std::invoke_result_t<Close, State&>>);
        static_assert(std::same_as<ResultType, std::invoke_result_t<Finalize, State&>>);
        static_assert(std::same_as<ResultType, std::invoke_result_t<Flush, State&>>);

        auto result = std::invoke(std::forward<Release>(release), state);

        auto closeResult = std::invoke(std::forward<Close>(close), state);
        if (result)
        {
            result = std::move(closeResult);
        }

        auto finalizeResult = std::invoke(std::forward<Finalize>(finalize), state);
        if (result)
        {
            result = std::move(finalizeResult);
        }

        auto flushResult = std::invoke(std::forward<Flush>(flush), state);
        if (result)
        {
            result = std::move(flushResult);
        }

        return result;
    }
}
