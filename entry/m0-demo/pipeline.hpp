#pragma once

#include "guard.hpp"
#include "pacing.hpp"

#include <target-setup.hpp>

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

    inline constexpr auto k_defaultTransitionTimeout = MonotonicInstant::Duration{
        std::chrono::seconds{5}
    };

    struct Template final
    {
        std::string            label{};
        std::vector<std::byte> gray{};
        uint32                 width{};
        uint32                 height{};
        Rect<FrameSpace>       roi;
    };

    struct Templates final
    {
        Template home;
        Template result;
        Template reset;
    };

    struct LoopConfig final
    {
        uint32                     loops{};
        uint64                     threshold{};
        MonotonicInstant::Duration maxActionFrameAge{};
        MonotonicInstant::Duration transitionTimeout{};
        GuardPolicy                guardPolicy{};
        std::optional<ClickDelay>  clickDelay{};
        uint64                     seed{};
    };

    struct RunSummary final
    {
        uint32 attempted{};
        uint32 succeeded{};
        uint32 guardViolations{};
        bool   stopped{};
        bool   auditClean{};

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
        std::size_t delivered{};
        bool        allToTarget{};
        bool        allAllowed{};

        auto operator==(AuditSummary const&) const -> bool = default;

        [[nodiscard]]
        constexpr auto isClean() const noexcept -> bool
        {
            return allToTarget && allAllowed;
        }
    };

    [[nodiscard]]
    auto loadTemplate(
        std::filesystem::path const& path,
        std::string label,
        Rect<FrameSpace> roi
    ) -> Result<Template>;

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
