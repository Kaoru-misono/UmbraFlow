#pragma once

#include "controller/discovery.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

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
        std::optional<uint32> win32Error{};
        bool                  isPerMonitorAwareV2{};
    };

    [[nodiscard]] auto enumerateCandidates() -> Result<std::vector<TargetCandidate>>;
    [[nodiscard]] auto windowClientSize(WindowHandle handle) -> Result<ClientSize>;
    [[nodiscard]] auto windowIsAlive(WindowHandle handle) noexcept -> bool;
    [[nodiscard]] auto windowProcess(WindowHandle handle) -> Result<ProcessId>;
    [[nodiscard]]
    auto processStartTime(
        ProcessId process
    ) -> Result<std::optional<ProcessStartTime>>;
    [[nodiscard]] auto setPerMonitorAwareV2() noexcept -> DpiSetObservation;
    [[nodiscard]] auto scanCodeFor(uint16 virtualKey) noexcept -> uint8;
    [[nodiscard]]
    auto postInputMessage(
        WindowHandle windowHandle,
        controller_detail::PostSpec spec,
        AuditLog& audit
    ) -> Status;
}
