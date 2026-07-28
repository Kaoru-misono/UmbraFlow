#pragma once

#include "controller/input.hpp"

#include <core/types/integer.hpp>

namespace uf::controller_detail
{
    struct AuditLogAccess final
    {
        static auto record(
            AuditLog& audit,
            WindowHandle windowHandle,
            uint32 message,
            uintptr wParam,
            intptr lParam
        ) -> void
        {
            audit.record(windowHandle, message, wParam, lParam);
        }
    };
}
