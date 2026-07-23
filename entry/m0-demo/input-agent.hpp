#pragma once

#include <controller/input.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>
#include <domain/detection.hpp>
#include <domain/space.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace uf::m0_demo
{
    class InputAgentQueueReader final
    {
        std::filesystem::path m_path;

        uintmax     m_offset{};
        std::string m_pending{};

        explicit InputAgentQueueReader(
            std::filesystem::path path
        ) noexcept;

        [[nodiscard]]
        auto extractLines() -> Result<std::vector<std::string>>;

    public:
        InputAgentQueueReader(InputAgentQueueReader const&) = delete;
        auto operator=(InputAgentQueueReader const&)
            -> InputAgentQueueReader& = delete;
        InputAgentQueueReader(InputAgentQueueReader&&) noexcept = default;
        auto operator=(InputAgentQueueReader&&) noexcept
            -> InputAgentQueueReader& = default;
        ~InputAgentQueueReader() = default;

        [[nodiscard]]
        static auto create(
            std::filesystem::path path
        ) -> Result<InputAgentQueueReader>;

        [[nodiscard]]
        auto readAvailable() -> Result<std::vector<std::string>>;
    };

    auto clearInputAgentCommandAudit(AuditLog& audit) noexcept -> void;

    [[nodiscard]]
    auto validateInputAgentClick(
        DeliveryTarget const& target,
        ObservationLease lease,
        Point<ClientSpace> point,
        MonotonicInstant now
    ) -> Status;

    [[nodiscard]]
    auto runInputAgent(
        std::span<std::string const> raw
    ) -> Status;
}
