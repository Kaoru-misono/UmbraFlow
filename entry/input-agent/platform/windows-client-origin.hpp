#pragma once

#include <controller/discovery.hpp>
#include <core/error/result.hpp>
#include <domain/space.hpp>

namespace uf::input_agent::platform
{
    // Where a window's client area starts on the desktop. A capture session is
    // built from it, so it is target setup rather than guard policy; it was
    // extracted from the M0 demo's guard boundary when the two split.
    [[nodiscard]]
    auto clientOriginDesktop(WindowHandle windowHandle) -> Result<Point<DesktopSpace>>;
}
