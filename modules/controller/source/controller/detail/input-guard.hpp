#pragma once

#include "controller/input.hpp"

#include <cstdint>

namespace uf::controller_detail
{
    struct AuditLogAccess final
    {
        static auto record(
            AuditLog& audit,
            WindowHandle windowHandle,
            std::uint32_t message,
            std::uintptr_t wParam,
            std::intptr_t lParam
        ) -> void
        {
            audit.record(windowHandle, message, wParam, lParam);
        }
    };
}
