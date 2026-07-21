#include "shutdown.hpp"

#include "platform/windows-console-control.hpp"

#include <utility>

namespace uf::m0_demo::detail
{
    class ConsoleControlRegistrationAccess final
    {
    public:
        [[nodiscard]]
        static auto create() noexcept -> ConsoleControlRegistration
        {
            return ConsoleControlRegistration{true};
        }
    };
}

namespace uf::m0_demo
{
    ConsoleControlRegistration::ConsoleControlRegistration(bool registered) noexcept
        : m_registered{registered}
    {
    }

    ConsoleControlRegistration::ConsoleControlRegistration(
        ConsoleControlRegistration&& other
    ) noexcept
        : m_registered{std::exchange(other.m_registered, false)}
    {
    }

    ConsoleControlRegistration::~ConsoleControlRegistration()
    {
        if (m_registered)
        {
            // Destruction is the best-effort fallback for an early-return path.
            // A normal application exit calls close() and propagates its error.
            try
            {
                static_cast<void>(platform::uninstallConsoleControlHandler());
            }
            catch (...)
            {
            }
        }
    }

    auto ConsoleControlRegistration::close() -> Status
    {
        if (!m_registered)
        {
            return ok();
        }

        UF_TRY(platform::uninstallConsoleControlHandler());
        m_registered = false;
        return ok();
    }

    auto installConsoleControlHandler() -> Result<ConsoleControlRegistration>
    {
        UF_TRY(platform::installConsoleControlHandler());
        return detail::ConsoleControlRegistrationAccess::create();
    }

    auto stopRequested() noexcept -> bool
    {
        return platform::stopRequested();
    }
}
