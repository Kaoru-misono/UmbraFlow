#pragma once

#include <core/error/result.hpp>

#include <stop_token>
#include <utility>

namespace uf::cli::platform
{
    // Installs a console Ctrl-C / Ctrl-Break handler that requests stop on a
    // process-lifetime stop_source, and removes it on destruction. Exploration
    // observes cancellation through token(); the OS callback cannot capture, so
    // the source is module-static and this RAII type owns only the registration.
    class ConsoleCancellation final
    {
        bool m_active{true};

        ConsoleCancellation() noexcept = default;

    public:
        ConsoleCancellation(ConsoleCancellation const&)                    = delete;
        auto operator=(ConsoleCancellation const&) -> ConsoleCancellation& = delete;

        ConsoleCancellation(ConsoleCancellation&& other) noexcept
            : m_active{std::exchange(other.m_active, false)}
        {
        }

        auto operator=(ConsoleCancellation&&) -> ConsoleCancellation& = delete;

        ~ConsoleCancellation();

        [[nodiscard]] static auto install() -> Result<ConsoleCancellation>;

        // True once a console Ctrl-C / Ctrl-Break has requested stop on the
        // process-lifetime source. It stays readable after the RAII registration
        // has been released, since the source outlives it, so the exit-code
        // boundary can report cancellation even though the handler is already gone.
        [[nodiscard]] static auto stopRequested() noexcept -> bool;

        [[nodiscard]] auto token() const noexcept -> std::stop_token;
    };
}
