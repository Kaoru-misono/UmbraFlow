#include "input.hpp"

#include <core/types/integer.hpp>

#include <span>

namespace uf
{
    auto AuditLog::record(
        WindowHandle windowHandle,
        uint32 message,
        uintptr wParam,
        intptr lParam
    ) -> void
    {
        m_records.emplace_back(
            AuditRecord{
                .target  = static_cast<uintptr>(windowHandle.value()),
                .message = message,
                .wParam  = wParam,
                .lParam  = lParam,
                .at      = MonotonicInstant::now(),
            }
        );
    }

    auto AuditLog::records() const noexcept -> std::span<AuditRecord const>
    {
        return m_records;
    }

    auto AuditLog::size() const noexcept -> std::size_t { return m_records.size(); }
    auto AuditLog::empty() const noexcept -> bool { return m_records.empty(); }
}
