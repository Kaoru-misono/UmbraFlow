#pragma once

#include <core/error/result.hpp>

namespace uf::m0_demo::platform
{
    [[nodiscard]] auto installConsoleControlHandler() -> Status;
    [[nodiscard]] auto uninstallConsoleControlHandler() -> Status;
    [[nodiscard]] auto stopRequested() noexcept -> bool;
}
