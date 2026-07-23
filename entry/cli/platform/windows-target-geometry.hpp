#pragma once

#include <controller/discovery.hpp>
#include <core/error/result.hpp>

#include <domain/space.hpp>

namespace uf::cli::platform
{
    // Resolves the desktop-space origin of a window's client area. The capture
    // session needs it to build ClientGeometry from the resolved target.
    [[nodiscard]]
    auto clientOriginDesktop(WindowHandle windowHandle) -> Result<Point<DesktopSpace>>;
}
