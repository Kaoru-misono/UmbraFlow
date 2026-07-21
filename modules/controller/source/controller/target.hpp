#pragma once

#include "discovery.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <domain/ids.hpp>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace uf
{
    class TargetSelector final
    {
        std::optional<ProcessId> m_process;
        std::optional<WindowHandle> m_windowHandle;
        std::optional<std::string> m_windowClass;
        std::optional<std::string> m_title;

    public:
        auto operator==(TargetSelector const&) const -> bool = default;

        [[nodiscard]] auto withProcess(ProcessId process) const -> TargetSelector;
        [[nodiscard]]
        auto withWindowHandle(WindowHandle windowHandle) const -> TargetSelector;
        [[nodiscard]] auto withWindowClass(std::string windowClass) const -> TargetSelector;
        [[nodiscard]] auto withTitle(std::string title) const -> TargetSelector;

        [[nodiscard]] auto process() const noexcept -> std::optional<ProcessId>;
        [[nodiscard]] auto windowHandle() const noexcept -> std::optional<WindowHandle>;
        [[nodiscard]]
        auto windowClass() const noexcept UF_LIFETIME_BOUND
            -> std::optional<std::string> const&;
        [[nodiscard]]
        auto title() const noexcept UF_LIFETIME_BOUND -> std::optional<std::string> const&;

        [[nodiscard]] auto matches(TargetCandidate const& candidate) const noexcept -> bool;
    };

    [[nodiscard]]
    auto matchingCandidates(
        std::span<TargetCandidate const> candidates,
        TargetSelector const& selector
    ) -> std::vector<TargetCandidate>;

    class TargetIdentity final
    {
        WindowHandle m_handle;
        ProcessId m_process;
        std::optional<ProcessStartTime> m_processStartTime;
        ClientSize m_clientSize;

    public:
        constexpr TargetIdentity(
            WindowHandle handle,
            ProcessId process,
            std::optional<ProcessStartTime> processStartTime,
            ClientSize clientSize
        ) noexcept
            : m_handle{handle}
            , m_process{process}
            , m_processStartTime{processStartTime}
            , m_clientSize{clientSize}
        {
        }

        auto operator==(TargetIdentity const&) const -> bool = default;

        [[nodiscard]] constexpr auto handle() const noexcept -> WindowHandle { return m_handle; }
        [[nodiscard]] constexpr auto process() const noexcept -> ProcessId { return m_process; }
        [[nodiscard]]
        constexpr auto processStartTime() const noexcept -> std::optional<ProcessStartTime>
        {
            return m_processStartTime;
        }
        [[nodiscard]]
        constexpr auto clientSize() const noexcept -> ClientSize { return m_clientSize; }
    };

    enum class RevalidateOutcome
    {
        Unchanged,
        GenerationBumped,
        InstanceUnconfirmed,
        Lost,
    };

    [[nodiscard]]
    auto errorOnLost(RevalidateOutcome outcome) -> Result<RevalidateOutcome>;

    class ResolvedTarget final
    {
        enum class Continuity
        {
            Confirmed,
            Lost,
            InstanceUnconfirmed,
        };

        friend auto resolveTarget(
            std::span<TargetCandidate const> candidates,
            TargetSelector const& selector
        ) -> Result<ResolvedTarget>;

        TargetIdentity m_identity;
        TargetGeneration m_generation;
        Continuity m_continuity;

        explicit ResolvedTarget(TargetIdentity identity) noexcept;

        [[nodiscard]] auto invalidate(Continuity continuity) -> Status;
        [[nodiscard]] auto readLiveIdentity() const -> std::optional<TargetIdentity>;

    public:
        [[nodiscard]] auto windowHandle() const noexcept -> WindowHandle;
        [[nodiscard]] auto clientSize() const noexcept -> ClientSize;
        [[nodiscard]] auto currentGeneration() const noexcept -> TargetGeneration;
        [[nodiscard]]
        auto identity() const noexcept UF_LIFETIME_BOUND -> TargetIdentity const&;
        [[nodiscard]] auto requiresReresolution() const noexcept -> bool;

        [[nodiscard]]
        auto reResolve(
            std::span<TargetCandidate const> candidates,
            TargetSelector const& selector
        ) -> Status;
        [[nodiscard]]
        auto applyRevalidation(
            std::optional<TargetIdentity> observed
        ) -> Result<RevalidateOutcome>;
        [[nodiscard]] auto revalidate() -> Result<RevalidateOutcome>;
    };

    [[nodiscard]]
    auto resolveTarget(
        std::span<TargetCandidate const> candidates,
        TargetSelector const& selector
    ) -> Result<ResolvedTarget>;
}
