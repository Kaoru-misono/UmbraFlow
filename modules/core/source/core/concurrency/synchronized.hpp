#pragma once

#include <concepts>
#include <functional>
#include <mutex>
#include <type_traits>
#include <utility>

namespace uf
{
    namespace detail
    {
        template <typename Result>
        constexpr auto g_lockResultDoesNotExposeStorage = (
            !std::is_reference_v<Result>
            && !std::is_pointer_v<std::remove_cv_t<Result>>
        );
    }

    template <typename Value, typename Mutex = std::mutex>
    class Synchronized final
    {
        Value         m_value{};
        mutable Mutex m_mutex;

    public:
        Synchronized()
            requires std::default_initializable<Value>
        = default;

        explicit Synchronized(Value value) noexcept(std::is_nothrow_move_constructible_v<Value>)
            : m_value{std::move(value)}
        {
        }

        template <typename... Arguments>
        explicit Synchronized(std::in_place_t, Arguments&&... arguments) noexcept(
            std::is_nothrow_constructible_v<Value, Arguments...>
        )
            : m_value(std::forward<Arguments>(arguments)...)
        {
        }

        Synchronized(Synchronized const&) = delete;
        auto operator=(Synchronized const&) -> Synchronized& = delete;
        Synchronized(Synchronized&&) = delete;
        auto operator=(Synchronized&&) -> Synchronized& = delete;

        template <typename Function>
            requires std::invocable<Function, Value&>
        auto withLock(Function&& function) -> std::invoke_result_t<Function, Value&>
        {
            using Result = std::invoke_result_t<Function, Value&>;
            static_assert(
                detail::g_lockResultDoesNotExposeStorage<Result>,
                "A synchronized operation cannot return a pointer or reference to protected storage."
            );

            auto lock = std::lock_guard<Mutex>{m_mutex};
            return std::invoke(std::forward<Function>(function), m_value);
        }

        template <typename Function>
            requires std::invocable<Function, Value const&>
        auto withLock(Function&& function) const -> std::invoke_result_t<Function, Value const&>
        {
            using Result = std::invoke_result_t<Function, Value const&>;
            static_assert(
                detail::g_lockResultDoesNotExposeStorage<Result>,
                "A synchronized operation cannot return a pointer or reference to protected storage."
            );

            auto lock = std::lock_guard<Mutex>{m_mutex};
            return std::invoke(std::forward<Function>(function), m_value);
        }
    };
}
