#include "shutdown.hpp"

#include "platform/windows-console-control.hpp"

namespace uf::m0_demo
{
    auto installConsoleControlHandler() -> Status
    {
        return platform::installConsoleControlHandler();
    }

    auto stopRequested() noexcept -> bool
    {
        return platform::stopRequested();
    }
}
