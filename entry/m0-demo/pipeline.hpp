#pragma once

#include "guard.hpp"
#include "pacing.hpp"

#include <controller/capture.hpp>
#include <controller/discovery.hpp>
#include <controller/input.hpp>
#include <controller/target.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <domain/space.hpp>
#include <vision/sad.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace uf::m0_demo
{
    class JsonlLog;

    inline constexpr auto g_defaultTransitionTimeout = MonotonicInstant::Duration{
        std::chrono::seconds{5}
    };

    struct Template final
    {
        std::string m_label;
        std::vector<std::byte> m_gray;
        std::uint32_t m_width;
        std::uint32_t m_height;
        Rect<FrameSpace> m_roi;
    };

    struct Templates final
    {
        Template m_home;
        Template m_result;
        Template m_reset;
    };

    struct LoopConfig final
    {
        std::uint32_t m_loops;
        std::uint64_t m_threshold;
        MonotonicInstant::Duration m_maxActionFrameAge;
        MonotonicInstant::Duration m_transitionTimeout;
        GuardPolicy m_guardPolicy;
        std::optional<ClickDelay> m_clickDelay;
        std::uint64_t m_seed;
    };

    struct RunSummary final
    {
        std::uint32_t m_attempted;
        std::uint32_t m_succeeded;
        std::uint32_t m_guardViolations;
        bool m_stopped;
        bool m_auditClean;

        auto operator==(RunSummary const&) const -> bool = default;

        [[nodiscard]] auto passed() const noexcept -> bool;
    };

    enum class StepStatus
    {
        Done,
        TimedOut,
        Failed,
        Stopped,
    };

    enum class LoopStatus
    {
        Success,
        Failed,
        GuardViolation,
        FailedAndGuardViolation,
        Stopped,
    };

    struct AuditSummary final
    {
        std::size_t m_delivered;
        bool m_allToTarget;
        bool m_allAllowed;

        auto operator==(AuditSummary const&) const -> bool = default;

        [[nodiscard]]
        constexpr auto isClean() const noexcept -> bool
        {
            return m_allToTarget && m_allAllowed;
        }
    };

    [[nodiscard]]
    auto loadTemplate(
        std::filesystem::path const& path,
        std::string label,
        Rect<FrameSpace> roi
    ) -> Result<Template>;

    [[nodiscard]] auto rgbaToBgra(std::vector<std::byte> rgba) -> std::vector<std::byte>;

    [[nodiscard]] auto ensureClientAreaUsable(ClientSize client) -> Status;

    [[nodiscard]] auto requireUnchangedTarget(RevalidateOutcome outcome) -> Status;

    [[nodiscard]]
    auto hitCenterFrame(
        SadMatch matched,
        std::uint32_t templateWidth,
        std::uint32_t templateHeight
    ) noexcept -> Point<FrameSpace>;

    [[nodiscard]]
    auto acceptMatch(
        std::optional<SadMatch> found,
        std::uint32_t templateWidth,
        std::uint32_t templateHeight,
        std::uint64_t maximumAverageSad
    ) noexcept -> std::optional<SadMatch>;

    [[nodiscard]] auto buildSelector(SelectorArgs const& selector) -> TargetSelector;

    [[nodiscard]]
    auto cropBgra(
        std::span<std::byte const> source,
        std::size_t stride,
        PixelRect rect
    ) -> Result<std::vector<std::byte>>;

    [[nodiscard]]
    auto ensureRoiInFrame(
        CoordinateTransform const& transform,
        std::string_view label,
        Rect<FrameSpace> roi
    ) -> Result<PixelRect>;

    [[nodiscard]]
    auto ensureTemplateFitsRoi(
        CoordinateTransform const& transform,
        Template const& imageTemplate
    ) -> Result<PixelRect>;

    [[nodiscard]]
    auto frameIsCausal(
        MonotonicInstant capturedAt,
        MonotonicInstant notBefore
    ) noexcept -> bool;

    [[nodiscard]]
    auto combineLoopStatus(StepStatus steps, bool guardPassed) noexcept -> LoopStatus;

    [[nodiscard]]
    auto summarizeAudit(
        std::span<AuditRecord const> records,
        std::uintptr_t target
    ) noexcept -> AuditSummary;

    [[nodiscard]]
    auto runPipeline(
        ResolvedTarget resolved,
        WgcCaptureSession session,
        DeliveryTarget delivery,
        Templates const& templates,
        LoopConfig const& config,
        JsonlLog& log
    ) -> Result<RunSummary>;
}
