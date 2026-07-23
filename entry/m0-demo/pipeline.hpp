#pragma once

#include "guard.hpp"
#include "pacing.hpp"
#include "target-setup.hpp"

#include <controller/capture.hpp>
#include <controller/discovery.hpp>
#include <controller/input.hpp>
#include <controller/target.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>
#include <domain/space.hpp>
#include <vision/sad.hpp>

#include <chrono>
#include <cstddef>
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
        std::string            m_label{};
        std::vector<std::byte> m_gray{};
        uint32                 m_width{};
        uint32                 m_height{};
        Rect<FrameSpace>       m_roi;
    };

    struct Templates final
    {
        Template m_home;
        Template m_result;
        Template m_reset;
    };

    struct LoopConfig final
    {
        uint32                     m_loops{};
        uint64                     m_threshold{};
        MonotonicInstant::Duration m_maxActionFrameAge{};
        MonotonicInstant::Duration m_transitionTimeout{};
        GuardPolicy                m_guardPolicy{};
        std::optional<ClickDelay>  m_clickDelay{};
        uint64                     m_seed{};
    };

    struct RunSummary final
    {
        uint32 m_attempted{};
        uint32 m_succeeded{};
        uint32 m_guardViolations{};
        bool   m_stopped{};
        bool   m_auditClean{};

        auto operator==(RunSummary const&) const -> bool = default;

        [[nodiscard]] auto passed() const noexcept -> bool;
    };

    enum class StepStatus : uint8
    {
        Done,
        TimedOut,
        Failed,
        Stopped,
    };

    enum class LoopStatus : uint8
    {
        Success,
        Failed,
        GuardViolation,
        FailedAndGuardViolation,
        Stopped,
    };

    // How a click-delivery failure is triaged. The fallback for an unlisted
    // kind is deliberately FailStep and not AbortRun: a deterministic rejection
    // retried every frame would only spin to the transition timeout and
    // mislabel a certain rejection as a timeout, so the loop fails immediately
    // with the true kind recorded. Only a post that could not be queued at all
    // is fatal to the whole run.
    //
    // The catch-all is why this names the two kinds it special-cases instead of
    // switching exhaustively over AutomationErrorKind. failureResponse has no
    // meaningful default, so a new kind must not compile until someone places
    // it; here the default is itself the decision, so a new kind landing on
    // FailStep is the documented outcome rather than an omission. The
    // asymmetry is intentional.
    enum class ClickFailureDisposition : uint8
    {
        Retry,
        FailStep,
        AbortRun,
    };

    [[nodiscard]]
    auto clickFailureDisposition(Error const& error) noexcept -> ClickFailureDisposition;

    struct AuditSummary final
    {
        std::size_t m_delivered{};
        bool        m_allToTarget{};
        bool        m_allAllowed{};

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

    [[nodiscard]] auto requireUnchangedTarget(RevalidateOutcome outcome) -> Status;

    [[nodiscard]]
    auto hitCenterFrame(
        SadMatch matched,
        uint32 templateWidth,
        uint32 templateHeight
    ) noexcept -> Point<FrameSpace>;

    [[nodiscard]]
    auto acceptMatch(
        std::optional<SadMatch> found,
        uint32 templateWidth,
        uint32 templateHeight,
        uint64 maximumAverageSad
    ) noexcept -> std::optional<SadMatch>;

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
        uintptr target
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
