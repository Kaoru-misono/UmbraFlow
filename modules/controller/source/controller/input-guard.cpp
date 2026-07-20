#include "input.hpp"

#include <span>

namespace uf
{
    auto AuditLog::record(
        WindowHandle windowHandle,
        std::uint32_t message,
        std::uintptr_t wParam,
        std::intptr_t lParam
    ) -> void
    {
        m_records.emplace_back(
            AuditRecord{
                .m_target = static_cast<std::uintptr_t>(windowHandle.value()),
                .m_message = message,
                .m_wParam = wParam,
                .m_lParam = lParam,
                .m_at = MonotonicInstant::now(),
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
