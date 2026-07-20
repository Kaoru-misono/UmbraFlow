#pragma once

#include "guard.hpp"

#include <controller/discovery.hpp>
#include <core/error/result.hpp>
#include <domain/space.hpp>

#include <optional>

namespace uf::m0_demo::platform
{
    [[nodiscard]] auto observeGuard(GuardPolicy policy) -> Result<GuardBaseline>;

    [[nodiscard]]
    auto clientOriginDesktop(WindowHandle windowHandle) -> Result<Point<DesktopSpace>>;

    [[nodiscard]] auto currentProcessIntegrity() -> std::optional<IntegrityLevel>;
    [[nodiscard]] auto processIntegrity(ProcessId process) -> std::optional<IntegrityLevel>;
}
