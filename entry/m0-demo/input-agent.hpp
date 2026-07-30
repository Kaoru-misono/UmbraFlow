#pragma once

#include <controller/input.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>
#include <domain/detection.hpp>
#include <domain/space.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

    struct InputAgentFramePaths final
    {
        std::filesystem::path before{};
        std::filesystem::path after{};

        auto operator==(InputAgentFramePaths const&) const -> bool = default;
    };

    // One action op's outcome exactly as its results line reports it: the two
    // frames that bracket the action, whether the input actually reached the
    // target, and one error field. delivered is the distinction every caller
    // depends on, so a rejected op must never report it true.
    struct InputAgentActionResult final
    {
        std::optional<uint64> beforeFrame{};
        std::optional<uint64> afterFrame{};
        bool                  delivered{};
        std::optional<Error>  error{};
    };

    auto clearInputAgentCommandAudit(AuditLog& audit) noexcept -> void;

    // Resolves the before-frame and after-frame outputs of one action op. Both
    // must stay inside the canonical output directory, must not alias each other
    // or either IPC file, and must not exist yet: an input-agent output is
    // always a fresh file, so a caller can never mistake a stale frame on disk
    // for the one this action produced.
    [[nodiscard]]
    auto resolveInputAgentFramePaths(
        std::filesystem::path const& outputBefore,
        std::filesystem::path const& outputAfter,
        std::filesystem::path const& canonicalOutputDirectory,
        std::filesystem::path const& canonicalQueue,
        std::filesystem::path const& canonicalResults,
        std::string_view operation
    ) -> Result<InputAgentFramePaths>;

    // click and key share this line shape, because both frame one delivered
    // action between a before-frame and an after-frame.
    [[nodiscard]]
    auto serializeInputAgentActionResult(
        std::string_view operation,
        InputAgentActionResult const& result
    ) -> std::string;

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
