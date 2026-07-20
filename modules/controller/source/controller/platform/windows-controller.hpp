#pragma once

#include "controller/discovery.hpp"

#include <core/error/result.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace uf
{
    class AuditLog;

    namespace controller_detail
    {
        struct PostSpec;
    }
}

namespace uf::controller_platform
{
    struct DpiSetObservation final
    {
        std::optional<std::uint32_t> m_win32Error;
        bool m_isPerMonitorAwareV2;
    };

    [[nodiscard]] auto enumerateCandidates() -> Result<std::vector<TargetCandidate>>;
    [[nodiscard]] auto windowClientSize(WindowHandle handle) -> Result<ClientSize>;
    [[nodiscard]] auto windowIsAlive(WindowHandle handle) noexcept -> bool;
    [[nodiscard]] auto windowProcess(WindowHandle handle) noexcept -> ProcessId;
    [[nodiscard]]
    auto processStartTime(ProcessId process) -> std::optional<ProcessStartTime>;
    [[nodiscard]] auto setPerMonitorAwareV2() noexcept -> DpiSetObservation;
    [[nodiscard]] auto scanCodeFor(std::uint16_t virtualKey) noexcept -> std::uint8_t;
    [[nodiscard]]
    auto postInputMessage(
        WindowHandle windowHandle,
        controller_detail::PostSpec spec,
        AuditLog& audit
    ) -> Status;
}
