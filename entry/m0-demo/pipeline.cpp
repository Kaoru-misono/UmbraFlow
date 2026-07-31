#include "pipeline.hpp"

#include "log-jsonl.hpp"
#include <error-text.hpp>
#include "platform/windows-background-messages.hpp"
#include "shutdown.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>
#include <domain/detection.hpp>
#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <image/pixels.hpp>
#include <image/png.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace uf::m0_demo
{
    // Borrowed from the input agent, which owns the shared entry substrate
    // this frozen demo was split away from.
    using input_agent::formatAutomationError;
    using input_agent::requireUnchangedTarget;

    namespace
    {
        constexpr auto k_bgraBytesPerPixel = std::size_t{4};
        constexpr auto k_maximumSadPixelComparisons = (
            uint64{64} * 1024U * 1024U
        );

        class Machine final
        {
        public:
            ResolvedTarget    m_resolved;
            WgcCaptureSession m_session;
            DeliveryTarget    m_target;
            HeldInputs        m_held{};
            AuditLog          m_audit{};

            Machine(
                ResolvedTarget resolved,
                WgcCaptureSession session,
                DeliveryTarget target
            )
                : m_resolved{resolved}
                , m_session{std::move(session)}
                , m_target{target}
            {
            }

            [[nodiscard]] auto ensureTargetUnchanged() -> Status
            {
                UF_TRY(m_session.validateTargetInstance());
                UF_TRY_VALUE(outcome, m_resolved.revalidate());
                UF_TRY(requireUnchangedTarget(outcome));
                return m_session.validateTargetInstance();
            }

            [[nodiscard]]
            auto cleanupTarget() -> Result<std::pair<DeliveryTarget, std::optional<Error>>>
            {
                auto hasFreshIdentity  = false;
                auto revalidationError = std::optional<Error>{};
                auto instance          = m_session.validateTargetInstance();
                if (!instance)
                {
                    revalidationError = std::move(instance).error();
                }
                else
                {
                    auto revalidated = m_resolved.revalidate();
                    if (revalidated)
                    {
                        auto unchanged = requireUnchangedTarget(*revalidated);
                        if (!unchanged)
                        {
                            revalidationError = std::move(unchanged).error();
                        }
                        else
                        {
                            auto confirmed = m_session.validateTargetInstance();
                            if (confirmed)
                            {
                                hasFreshIdentity = true;
                            }
                            else
                            {
                                revalidationError = std::move(confirmed).error();
                            }
                        }
                    }
                    else
                    {
                        revalidationError = std::move(revalidated).error();
                    }
                }

                auto windowHandle = m_target.windowHandle();
                auto sessionId    = m_target.sessionId();
                auto generation   = m_target.generation();
                if (hasFreshIdentity)
                {
                    windowHandle = m_resolved.windowHandle();
                    generation   = m_resolved.currentGeneration();
                }
                else
                {
                    sessionId = CaptureSessionId{~m_target.sessionId().value()};
                }

                UF_TRY_VALUE(
                    cleanup,
                    DeliveryTarget::create(
                        windowHandle,
                        sessionId,
                        generation,
                        m_target.clientWidth(),
                        m_target.clientHeight()
                    )
                );
                return std::pair{
                    cleanup,
                    std::move(revalidationError)
                };
            }
        };

        using ClickStatus = std::variant<MonotonicInstant, StepStatus>;

        [[nodiscard]]
        auto recognizeRaw(
            Frame const& frame,
            Template const& imageTemplate,
            MonotonicInstant started,
            MonotonicInstant::Duration timeout
        ) -> Result<SadSearchOutcome>
        {
            auto const transform = frame.transform();
            UF_TRY_VALUE(roi, transform.frameRectToPixelRect(imageTemplate.roi));
            auto const pixels = frame.pixels();
            UF_TRY_VALUE(
                roiBgra,
                image::cropBgra8(
                    pixels->bytes(),
                    frame.width(),
                    frame.height(),
                    frame.stride(),
                    roi
                )
            );

            auto const roiWidth = checkedCast<std::size_t>(roi.width());
            if (!roiWidth)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "ROI width is not addressable"
                );
            }
            auto const roiRowBytes = checkedMultiply(
                *roiWidth,
                k_bgraBytesPerPixel
            );
            if (!roiRowBytes)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "ROI row byte count overflowed"
                );
            }

            UF_TRY_VALUE(
                gray,
                bgra8ToGray8(
                    roiBgra,
                    roi.width(),
                    roi.height(),
                    *roiRowBytes
                )
            );
            UF_TRY_VALUE(
                haystack,
                GrayImage::create(
                    gray,
                    roi.width(),
                    roi.height(),
                    *roiWidth
                )
            );

            auto const templateWidth = checkedCast<std::size_t>(imageTemplate.width);
            if (!templateWidth)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "template width is not addressable"
                );
            }
            UF_TRY_VALUE(
                templateImage,
                GrayImage::create(
                    imageTemplate.gray,
                    imageTemplate.width,
                    imageTemplate.height,
                    *templateWidth
                )
            );
            UF_TRY_VALUE(search, PixelRect::create(0, 0, roi.width(), roi.height()));
            auto const poll = SadSearchPoll{
                [started, timeout]() noexcept -> SadSearchControl
                {
                    if (stopRequested())
                    {
                        return SadSearchControl::Cancelled;
                    }
                    if (
                        MonotonicInstant::now().saturatingDurationSince(started)
                        >= timeout
                    )
                    {
                        return SadSearchControl::TimedOut;
                    }
                    return SadSearchControl::Continue;
                }
            };
            UF_TRY_VALUE(
                report,
                matchTemplateSad(
                    haystack,
                    templateImage,
                    search,
                    k_maximumSadPixelComparisons,
                    poll
                )
            );
            auto const& outcome = report.outcome;
            if (std::holds_alternative<SadSearchStopReason>(outcome))
            {
                return SadSearchOutcome{
                    std::get<SadSearchStopReason>(outcome)
                };
            }
            auto found = std::get<std::optional<SadMatch>>(outcome);
            if (!found)
            {
                return SadSearchOutcome{std::optional<SadMatch>{}};
            }

            auto const x = checkedAdd(found->x(), roi.x());
            auto const y = checkedAdd(found->y(), roi.y());
            if (!x || !y)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "recognized match coordinate overflowed"
                );
            }
            return SadSearchOutcome{
                std::optional<SadMatch>{SadMatch{*x, *y, found->score()}}
            };
        }

        [[nodiscard]]
        auto timedOut(
            MonotonicInstant started,
            MonotonicInstant::Duration timeout
        ) noexcept -> bool
        {
            return MonotonicInstant::now().saturatingDurationSince(started) >= timeout;
        }

        [[nodiscard]]
        auto logTimeout(
            JsonlLog& log,
            std::string_view phase,
            std::string_view label,
            uint32 loopIndex
        ) -> Status
        {
            return log.write(
                LogLine{std::string{phase}, "timeout"}
                    .withLoopIndex(loopIndex)
                    .withOutcome("timeout")
                    .withDetail(
                        std::format(
                            "{} did not appear within the transition timeout",
                            label
                        )
                    )
            );
        }

        [[nodiscard]]
        auto searchStopStatus(
            SadSearchOutcome const& outcome,
            JsonlLog& log,
            std::string_view phase,
            std::string_view label,
            uint32 loopIndex
        ) -> Result<std::optional<StepStatus>>
        {
            auto const* reason = std::get_if<SadSearchStopReason>(&outcome);
            if (reason == nullptr)
            {
                return std::optional<StepStatus>{};
            }

            switch (*reason)
            {
            case SadSearchStopReason::Cancelled:
                return std::optional{StepStatus::Stopped};
            case SadSearchStopReason::TimedOut:
                UF_TRY(logTimeout(log, phase, label, loopIndex));
                return std::optional{StepStatus::TimedOut};
            case SadSearchStopReason::ComparisonBudgetExhausted:
                UF_TRY(
                    log.write(
                        LogLine{std::string{phase}, "comparison_budget_exhausted"}
                            .withLoopIndex(loopIndex)
                            .withOutcome("failed")
                            .withDetail(
                                std::format(
                                    "{} exceeded the {} pixel-comparison search budget",
                                    label,
                                    k_maximumSadPixelComparisons
                                )
                            )
                    )
                );
                return std::optional{StepStatus::Failed};
            default:
                UF_UNREACHABLE_MSG("Unknown SadSearchStopReason value");
            }
        }

        [[nodiscard]]
        auto captureFresh(
            Machine& machine,
            std::string_view phase,
            std::string_view label,
            uint32 loopIndex,
            JsonlLog& log
        ) -> Result<std::optional<Frame>>
        {
            UF_TRY(machine.ensureTargetUnchanged());
            auto captured = machine.m_session.capture();
            if (captured)
            {
                UF_TRY(machine.ensureTargetUnchanged());
                return std::optional<Frame>{*std::move(captured)};
            }

            // Only CaptureStalled yields "no frame this round"; every other
            // capture failure terminates the run. StaleObservation must stay
            // fail-closed rather than be reported as a stall, so this triage
            // names the one kind it accepts instead of reading a shared policy
            // table. See docs/plans/2026-07-20-post-port-win32-robustness.md.
            if (
                automationErrorKind(captured.error())
                == AutomationErrorKind::CaptureStalled
            )
            {
                UF_TRY(
                    log.write(
                        LogLine{std::string{phase}, "capture_stalled"}
                            .withLoopIndex(loopIndex)
                            .withOutcome("stalled")
                            .withDetail(
                                std::format(
                                    "{}: {}",
                                    label,
                                    formatAutomationError(captured.error())
                                )
                            )
                    )
                );
                return std::optional<Frame>{};
            }
            return std::unexpected{std::move(captured).error()};
        }

        [[nodiscard]]
        auto setupValidateRois(
            Machine& machine,
            Templates const& templates,
            LoopConfig const& config,
            JsonlLog& log
        ) -> Status
        {
            auto const started = MonotonicInstant::now();
            auto transform = std::optional<CoordinateTransform>{};
            while (!transform)
            {
                if (stopRequested())
                {
                    return ok();
                }
                UF_TRY_VALUE(
                    frame,
                    captureFresh(machine, "setup", "roi_validation", 0, log)
                );
                if (frame)
                {
                    transform = frame->transform();
                    break;
                }
                if (timedOut(started, config.transitionTimeout))
                {
                    return fail(
                        AutomationErrorKind::CaptureStalled,
                        "no frame captured within the transition timeout to validate ROIs against"
                    );
                }
            }

            UF_TRY(ensureTemplateFitsRoi(*transform, templates.home));
            UF_TRY(ensureTemplateFitsRoi(*transform, templates.result));
            UF_TRY(ensureTemplateFitsRoi(*transform, templates.reset));
            auto const [frameWidth, frameHeight] = transform->frameSize();
            return log.write(
                LogLine{"setup", "roi_validated"}
                    .withOutcome("ok")
                    .withDetail(
                        std::format(
                            "three ROIs fit the {}x{} frame",
                            frameWidth,
                            frameHeight
                        )
                    )
            );
        }

        [[nodiscard]]
        auto clickWhenPresent(
            Machine& machine,
            Template const& imageTemplate,
            LoopConfig const& config,
            uint32 loopIndex,
            ClickPacer& pacer,
            JsonlLog& log
        ) -> Result<ClickStatus>
        {
            auto const label = std::string_view{imageTemplate.label};
            UF_TRY_VALUE(
                pace,
                pacer.pauseBeforeClick(label, loopIndex, log)
            );
            if (pace == PaceOutcome::Stopped)
            {
                return ClickStatus{StepStatus::Stopped};
            }

            auto const started = MonotonicInstant::now();
            while (true)
            {
                if (stopRequested())
                {
                    return ClickStatus{StepStatus::Stopped};
                }

                UF_TRY_VALUE(
                    captured,
                    captureFresh(machine, "action", label, loopIndex, log)
                );
                if (!captured)
                {
                    if (timedOut(started, config.transitionTimeout))
                    {
                        UF_TRY(logTimeout(log, "action", label, loopIndex));
                        return ClickStatus{StepStatus::TimedOut};
                    }
                    continue;
                }

                UF_TRY_VALUE(
                    raw,
                    recognizeRaw(
                        *captured,
                        imageTemplate,
                        started,
                        config.transitionTimeout
                    )
                );
                UF_TRY_VALUE(
                    searchStopped,
                    searchStopStatus(raw, log, "action", label, loopIndex)
                );
                if (searchStopped)
                {
                    return ClickStatus{*searchStopped};
                }
                auto const matched = acceptMatch(
                    std::get<std::optional<SadMatch>>(raw),
                    imageTemplate.width,
                    imageTemplate.height,
                    config.threshold
                );
                if (matched)
                {
                    auto const center = hitCenterFrame(
                        *matched,
                        imageTemplate.width,
                        imageTemplate.height
                    );
                    auto const point = captured->transform().frameToClient(center);
                    UF_TRY_VALUE(
                        lease,
                        ObservationLease::forFrame(
                            *captured,
                            config.maxActionFrameAge
                        )
                    );
                    UF_TRY(machine.ensureTargetUnchanged());
                    auto delivered = click(
                        machine.m_target,
                        lease,
                        point,
                        machine.m_held,
                        machine.m_audit
                    );
                    if (delivered)
                    {
                        auto const deliveredAt = MonotonicInstant::now();
                        UF_TRY(
                            log.write(
                                LogLine{"action", "click"}
                                    .withLoopIndex(loopIndex)
                                    .withFrame(*captured)
                                    .withConfidence(matched->score())
                                    .withLeaseOk(true)
                                    .withOutcome("ok")
                                    .withDetail(
                                        std::format(
                                            "clicked {} at client ({:.1f}, {:.1f})",
                                            label,
                                            point.x(),
                                            point.y()
                                        )
                                    )
                            )
                        );
                        return ClickStatus{deliveredAt};
                    }

                    auto const errorText = formatAutomationError(delivered.error());
                    switch (clickFailureDisposition(delivered.error()))
                    {
                    case ClickFailureDisposition::Retry:
                        UF_TRY(
                            log.write(
                                LogLine{"action", "click_retry"}
                                    .withLoopIndex(loopIndex)
                                    .withFrame(*captured)
                                    .withConfidence(matched->score())
                                    .withLeaseOk(false)
                                    .withOutcome("retry")
                                    .withDetail(std::format("{}: {}", label, errorText))
                            )
                        );
                        break;
                    case ClickFailureDisposition::FailStep:
                        UF_TRY(
                            log.write(
                                LogLine{"action", "click_error"}
                                    .withLoopIndex(loopIndex)
                                    .withFrame(*captured)
                                    .withConfidence(matched->score())
                                    .withLeaseOk(false)
                                    .withOutcome("error")
                                    .withDetail(std::format("{}: {}", label, errorText))
                            )
                        );
                        return ClickStatus{StepStatus::Failed};
                    case ClickFailureDisposition::AbortRun:
                        UF_TRY(
                            log.write(
                                LogLine{"action", "click_error"}
                                    .withLoopIndex(loopIndex)
                                    .withFrame(*captured)
                                    .withConfidence(matched->score())
                                    .withLeaseOk(false)
                                    .withOutcome("error")
                                    .withDetail(std::format("{}: {}", label, errorText))
                            )
                        );
                        return std::unexpected{std::move(delivered).error()};
                    }
                }

                if (timedOut(started, config.transitionTimeout))
                {
                    UF_TRY(logTimeout(log, "action", label, loopIndex));
                    return ClickStatus{StepStatus::TimedOut};
                }
            }
        }

        [[nodiscard]]
        auto waitUntilPresent(
            Machine& machine,
            Template const& imageTemplate,
            MonotonicInstant notBefore,
            LoopConfig const& config,
            uint32 loopIndex,
            JsonlLog& log
        ) -> Result<StepStatus>
        {
            auto const label = std::string_view{imageTemplate.label};
            auto const started = MonotonicInstant::now();
            while (true)
            {
                if (stopRequested())
                {
                    return StepStatus::Stopped;
                }

                UF_TRY_VALUE(
                    captured,
                    captureFresh(machine, "recognize", label, loopIndex, log)
                );
                if (!captured)
                {
                    if (timedOut(started, config.transitionTimeout))
                    {
                        UF_TRY(logTimeout(log, "recognize", label, loopIndex));
                        return StepStatus::TimedOut;
                    }
                    continue;
                }

                if (!frameIsCausal(captured->capturedAt(), notBefore))
                {
                    UF_TRY(
                        log.write(
                            LogLine{"recognize", "pre_action_frame_discarded"}
                                .withLoopIndex(loopIndex)
                                .withFrame(*captured)
                                .withOutcome("stale")
                                .withDetail(
                                    std::format(
                                        "{}: frame arrived before click completed",
                                        label
                                    )
                                )
                        )
                    );
                    if (timedOut(started, config.transitionTimeout))
                    {
                        UF_TRY(logTimeout(log, "recognize", label, loopIndex));
                        return StepStatus::TimedOut;
                    }
                    continue;
                }

                UF_TRY_VALUE(
                    raw,
                    recognizeRaw(
                        *captured,
                        imageTemplate,
                        started,
                        config.transitionTimeout
                    )
                );
                UF_TRY_VALUE(
                    searchStopped,
                    searchStopStatus(raw, log, "recognize", label, loopIndex)
                );
                if (searchStopped)
                {
                    return *searchStopped;
                }
                auto const matched = acceptMatch(
                    std::get<std::optional<SadMatch>>(raw),
                    imageTemplate.width,
                    imageTemplate.height,
                    config.threshold
                );
                if (matched)
                {
                    UF_TRY(
                        log.write(
                            LogLine{"recognize", "present"}
                                .withLoopIndex(loopIndex)
                                .withFrame(*captured)
                                .withConfidence(matched->score())
                                .withOutcome("ok")
                                .withDetail(std::format("{} present", label))
                        )
                    );
                    return StepStatus::Done;
                }

                if (timedOut(started, config.transitionTimeout))
                {
                    UF_TRY(logTimeout(log, "recognize", label, loopIndex));
                    return StepStatus::TimedOut;
                }
            }
        }

        [[nodiscard]]
        auto runLoopSteps(
            Machine& machine,
            Templates const& templates,
            LoopConfig const& config,
            uint32 loopIndex,
            ClickPacer& pacer,
            JsonlLog& log
        ) -> Result<StepStatus>
        {
            UF_TRY_VALUE(
                clickedHome,
                clickWhenPresent(
                    machine,
                    templates.home,
                    config,
                    loopIndex,
                    pacer,
                    log
                )
            );
            if (!std::holds_alternative<MonotonicInstant>(clickedHome))
            {
                return std::get<StepStatus>(clickedHome);
            }
            auto const homeClickedAt = std::get<MonotonicInstant>(clickedHome);

            UF_TRY_VALUE(
                resultShown,
                waitUntilPresent(
                    machine,
                    templates.result,
                    homeClickedAt,
                    config,
                    loopIndex,
                    log
                )
            );
            if (resultShown != StepStatus::Done)
            {
                return resultShown;
            }

            UF_TRY_VALUE(
                clickedReset,
                clickWhenPresent(
                    machine,
                    templates.reset,
                    config,
                    loopIndex,
                    pacer,
                    log
                )
            );
            if (!std::holds_alternative<MonotonicInstant>(clickedReset))
            {
                return std::get<StepStatus>(clickedReset);
            }
            auto const resetClickedAt = std::get<MonotonicInstant>(clickedReset);
            return waitUntilPresent(
                machine,
                templates.home,
                resetClickedAt,
                config,
                loopIndex,
                log
            );
        }

        [[nodiscard]]
        auto runOne(
            Machine& machine,
            Templates const& templates,
            LoopConfig const& config,
            uint32 loopIndex,
            ClickPacer& pacer,
            JsonlLog& log
        ) -> Result<LoopStatus>
        {
            UF_TRY(log.write(LogLine{"loop", "start"}.withLoopIndex(loopIndex)));
            UF_TRY_VALUE(baseline, observeGuard(config.guardPolicy));
            auto const targetWindow = machine.m_target.windowHandle().value();
            UF_TRY(
                log.write(
                    LogLine{"guard", "baseline"}
                        .withLoopIndex(loopIndex)
                        .withDetail(
                            std::format(
                                "target_hwnd={:#x} foreground={:#x} cursor=({}, {})",
                                static_cast<uintptr>(targetWindow),
                                static_cast<uintptr>(baseline.foreground),
                                baseline.cursor.first,
                                baseline.cursor.second
                            )
                        )
                )
            );

            auto const baselineCheck = checkGuard(
                config.guardPolicy,
                targetWindow,
                baseline,
                baseline
            );
            if (!baselineCheck.baselineBackgroundOk)
            {
                UF_TRY(
                    log.write(
                        LogLine{"guard", "precondition"}
                            .withLoopIndex(loopIndex)
                            .withOutcome("violation")
                            .withDetail(
                                std::format(
                                    "target_hwnd={:#x} baseline_foreground={:#x}; guard mode requires a non-target foreground window",
                                    static_cast<uintptr>(targetWindow),
                                    static_cast<uintptr>(baseline.foreground)
                                )
                            )
                    )
                );
                UF_TRY(
                    log.write(
                        LogLine{"loop", "end"}
                            .withLoopIndex(loopIndex)
                            .withOutcome("guard_violation")
                    )
                );
                return LoopStatus::GuardViolation;
            }

            UF_TRY_VALUE(
                steps,
                runLoopSteps(
                    machine,
                    templates,
                    config,
                    loopIndex,
                    pacer,
                    log
                )
            );
            if (steps == StepStatus::Stopped)
            {
                return LoopStatus::Stopped;
            }

            UF_TRY_VALUE(observed, observeGuard(config.guardPolicy));
            auto const check = checkGuard(
                config.guardPolicy,
                targetWindow,
                baseline,
                observed
            );
            UF_TRY(
                log.write(
                    LogLine{"guard", "check"}
                        .withLoopIndex(loopIndex)
                        .withOutcome(check.passed() ? "ok" : "violation")
                        .withDetail(
                            std::format(
                                "target_hwnd={:#x} baseline_foreground={:#x} observed_foreground={:#x} baseline_cursor=({}, {}) observed_cursor=({}, {}) baseline_background_ok={} foreground_ok={} cursor_ok={}",
                                static_cast<uintptr>(targetWindow),
                                static_cast<uintptr>(baseline.foreground),
                                static_cast<uintptr>(observed.foreground),
                                baseline.cursor.first,
                                baseline.cursor.second,
                                observed.cursor.first,
                                observed.cursor.second,
                                check.baselineBackgroundOk,
                                check.foregroundOk,
                                check.cursorOk
                            )
                        )
                )
            );

            auto const status = combineLoopStatus(steps, check.passed());
            auto outcome = std::string_view{};
            switch (status)
            {
            case LoopStatus::Success: outcome = "success"; break;
            case LoopStatus::Failed: outcome = "failed"; break;
            case LoopStatus::GuardViolation: outcome = "guard_violation"; break;
            case LoopStatus::FailedAndGuardViolation:
                outcome = "failed_guard_violation";
                break;
            case LoopStatus::Stopped: outcome = "stopped"; break;
            }
            UF_TRY(
                log.write(
                    LogLine{"loop", "end"}
                        .withLoopIndex(loopIndex)
                        .withOutcome(std::string{outcome})
                )
            );
            return status;
        }

        [[nodiscard]]
        auto runLoops(
            Machine& machine,
            Templates const& templates,
            LoopConfig const& config,
            JsonlLog& log
        ) -> Result<RunSummary>
        {
            UF_TRY(setupValidateRois(machine, templates, config, log));
            auto pacer           = ClickPacer{config.clickDelay, config.seed};
            auto attempted       = uint32{0};
            auto succeeded       = uint32{0};
            auto guardViolations = uint32{0};
            auto stopped         = false;

            for (auto loopIndex = uint32{0}; loopIndex < config.loops; ++loopIndex)
            {
                if (stopRequested())
                {
                    stopped = true;
                    break;
                }

                ++attempted;
                UF_TRY_VALUE(
                    status,
                    runOne(
                        machine,
                        templates,
                        config,
                        loopIndex,
                        pacer,
                        log
                    )
                );
                switch (status)
                {
                case LoopStatus::Success:
                    ++succeeded;
                    break;
                case LoopStatus::GuardViolation:
                case LoopStatus::FailedAndGuardViolation:
                    ++guardViolations;
                    break;
                case LoopStatus::Failed:
                    break;
                case LoopStatus::Stopped:
                    stopped = true;
                    break;
                }
                if (stopped)
                {
                    break;
                }
            }

            return RunSummary{
                .attempted       = attempted,
                .succeeded       = succeeded,
                .guardViolations = guardViolations,
                .stopped         = stopped,
                .auditClean      = false,
            };
        }

        auto retainFirstError(Status& current, Status next) -> void
        {
            if (current)
            {
                current = std::move(next);
            }
        }

        [[nodiscard]]
        auto describeHeldInput(HeldInput const& input) -> std::string
        {
            return std::visit(
                []<typename Input>(Input const& value) -> std::string
                {
                    if constexpr (std::same_as<Input, KeyInput>)
                    {
                        return std::format(
                            "key(vk={}, extended={})",
                            value.virtualKey(),
                            value.isExtended()
                        );
                    }
                    else
                    {
                        return std::format(
                            "pointer(left, x={}, y={})",
                            value.pixel().x(),
                            value.pixel().y()
                        );
                    }
                },
                input
            );
        }

        [[nodiscard]]
        auto shutdownMachine(
            Machine& machine,
            JsonlLog& log,
            Result<RunSummary>& outcome
        ) -> Status
        {
            return runShutdown(
                machine,
                [&log](Machine& current) -> Status
                {
                    UF_TRY_VALUE(cleanup, current.cleanupTarget());
                    auto [cleanupTarget, revalidationError] = std::move(cleanup);
                    auto releases = releaseHeld(
                        cleanupTarget,
                        current.m_held,
                        current.m_audit
                    );
                    auto result = revalidationError
                        ? Status{std::unexpected{std::move(*revalidationError)}}
                        : ok();

                    if (releases.empty())
                    {
                        retainFirstError(
                            result,
                            log.write(
                                LogLine{"shutdown", "release_held"}
                                    .withOutcome("ok")
                                    .withDetail("no held inputs")
                            )
                        );
                    }
                    for (auto const& release : releases)
                    {
                        auto const succeeded = release.result.has_value();
                        if (!succeeded)
                        {
                            retainFirstError(
                                result,
                                Status{std::unexpected{release.result.error().clone()}}
                            );
                        }
                        auto const releaseResult = succeeded
                            ? std::string{"Ok"}
                            : formatAutomationError(release.result.error());
                        retainFirstError(
                            result,
                            log.write(
                                LogLine{"shutdown", "release_held"}
                                    .withOutcome(succeeded ? "ok" : "error")
                                    .withDetail(
                                        describeHeldInput(release.input)
                                        + ": "
                                        + releaseResult
                                    )
                            )
                        );
                    }
                    return result;
                },
                [&log](Machine& current) -> Status
                {
                    auto result = current.m_session.close();
                    auto const succeeded = result.has_value();
                    retainFirstError(
                        result,
                        log.write(
                            LogLine{"shutdown", "session_closed"}
                                .withOutcome(succeeded ? "ok" : "error")
                                .withDetail(
                                    succeeded
                                        ? std::string{}
                                        : formatAutomationError(result.error())
                                )
                        )
                    );
                    return result;
                },
                [&log, &outcome](Machine& current) -> Status
                {
                    auto const audit = summarizeAudit(
                        current.m_audit.records(),
                        static_cast<uintptr>(current.m_target.windowHandle().value())
                    );
                    if (outcome)
                    {
                        outcome->auditClean = audit.isClean();
                    }

                    auto result = log.write(
                        LogLine{"audit", "summary"}
                            .withOutcome(audit.isClean() ? "ok" : "violation")
                            .withDetail(
                                std::format(
                                    "delivered={} all_to_target={} all_allowed={}",
                                    audit.delivered,
                                    audit.allToTarget,
                                    audit.allAllowed
                                )
                            )
                    );
                    if (outcome)
                    {
                        retainFirstError(
                            result,
                            log.write(
                                LogLine{"run", "summary"}
                                    .withOutcome(outcome->passed() ? "ok" : "partial")
                                    .withDetail(
                                        std::format(
                                            "attempted={} succeeded={} guard_violations={} stopped={} audit_clean={}",
                                            outcome->attempted,
                                            outcome->succeeded,
                                            outcome->guardViolations,
                                            outcome->stopped,
                                            outcome->auditClean
                                        )
                                    )
                            )
                        );
                    }
                    return result;
                },
                [&log](Machine&) -> Status
                {
                    return log.flush();
                }
            );
        }
    }

    auto RunSummary::passed() const noexcept -> bool
    {
        return (
            !stopped
            && auditClean
            && guardViolations == 0U
            && succeeded == attempted
        );
    }

    auto loadTemplate(
        std::filesystem::path const& path,
        std::string label,
        Rect<FrameSpace> roi
    ) -> Result<Template>
    {
        UF_TRY_VALUE(decoded, image::loadPng(path));
        if (decoded.width == 0U || decoded.height == 0U)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format("template {} has zero dimensions", path.string())
            );
        }

        UF_TRY_VALUE(bgra, image::rgba8ToBgra8(std::move(decoded.pixels)));
        auto const width = checkedCast<std::size_t>(decoded.width);
        if (!width)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format("template {} width is not addressable", path.string())
            );
        }
        auto const rowBytes = checkedMultiply(*width, k_bgraBytesPerPixel);
        if (!rowBytes)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format("template {} row byte count overflowed", path.string())
            );
        }

        auto gray = bgra8ToGray8(
            bgra,
            decoded.width,
            decoded.height,
            *rowBytes
        );
        if (!gray)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "template {} could not be grayscaled: {}",
                    path.string(),
                    formatAutomationError(gray.error())
                )
            );
        }
        return Template{
            .label  = std::move(label),
            .gray   = *std::move(gray),
            .width  = decoded.width,
            .height = decoded.height,
            .roi    = roi,
        };
    }

    auto hitCenterFrame(
        SadMatch matched,
        uint32 templateWidth,
        uint32 templateHeight
    ) noexcept -> Point<FrameSpace>
    {
        return Rect<FrameSpace>{
            static_cast<float>(matched.x()),
            static_cast<float>(matched.y()),
            static_cast<float>(templateWidth),
            static_cast<float>(templateHeight)
        }.center();
    }

    auto acceptMatch(
        std::optional<SadMatch> found,
        uint32 templateWidth,
        uint32 templateHeight,
        uint64 maximumAverageSad
    ) noexcept -> std::optional<SadMatch>
    {
        auto const area = (
            static_cast<uint64>(templateWidth)
            * static_cast<uint64>(templateHeight)
        );
        auto const budget = checkedMultiply(maximumAverageSad, area).value_or(
            std::numeric_limits<uint64>::max()
        );
        if (!found || area == 0U || found->score() > budget)
        {
            return std::nullopt;
        }
        return found;
    }

    auto ensureRoiInFrame(
        CoordinateTransform const& transform,
        std::string_view label,
        Rect<FrameSpace> roi
    ) -> Result<PixelRect>
    {
        auto converted = transform.frameRectToPixelRect(roi);
        if (converted)
        {
            return converted;
        }

        auto const [frameWidth, frameHeight] = transform.frameSize();
        return fail(
            AutomationErrorKind::InvalidResource,
            std::format(
                "--{}-roi ({}, {}, {}, {}) is outside the captured {}x{} frame: {}",
                label,
                roi.x(),
                roi.y(),
                roi.width(),
                roi.height(),
                frameWidth,
                frameHeight,
                formatAutomationError(converted.error())
            )
        );
    }

    auto ensureTemplateFitsRoi(
        CoordinateTransform const& transform,
        Template const& imageTemplate
    ) -> Result<PixelRect>
    {
        UF_TRY_VALUE(
            pixelRoi,
            ensureRoiInFrame(transform, imageTemplate.label, imageTemplate.roi)
        );
        if (
            imageTemplate.width > pixelRoi.width()
            || imageTemplate.height > pixelRoi.height()
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "{} template {}x{} does not fit --{}-roi pixel extent {}x{}",
                    imageTemplate.label,
                    imageTemplate.width,
                    imageTemplate.height,
                    imageTemplate.label,
                    pixelRoi.width(),
                    pixelRoi.height()
                )
            );
        }
        return pixelRoi;
    }

    auto frameIsCausal(
        MonotonicInstant capturedAt,
        MonotonicInstant notBefore
    ) noexcept -> bool
    {
        return capturedAt >= notBefore;
    }

    auto clickFailureDisposition(Error const& error) noexcept -> ClickFailureDisposition
    {
        auto const kind = automationErrorKind(error);
        if (kind == AutomationErrorKind::ControllerDisconnected)
        {
            // The post could not be queued at all, so no later frame can help.
            return ClickFailureDisposition::AbortRun;
        }
        if (kind == AutomationErrorKind::StaleObservation)
        {
            // The observation went stale between recognize and deliver; a fresh
            // frame is the normal recovery.
            return ClickFailureDisposition::Retry;
        }
        return ClickFailureDisposition::FailStep;
    }

    auto combineLoopStatus(StepStatus steps, bool guardPassed) noexcept -> LoopStatus
    {
        if (steps == StepStatus::Done)
        {
            return guardPassed ? LoopStatus::Success : LoopStatus::GuardViolation;
        }
        return guardPassed ? LoopStatus::Failed : LoopStatus::FailedAndGuardViolation;
    }

    auto summarizeAudit(
        std::span<AuditRecord const> records,
        uintptr target
    ) noexcept -> AuditSummary
    {
        return AuditSummary{
            .delivered = records.size(),
            .allToTarget = std::ranges::all_of(
                records,
                [target](AuditRecord const& record) noexcept
                {
                    return record.target == target;
                }
            ),
            .allAllowed = std::ranges::all_of(
                records,
                [](AuditRecord const& record) noexcept
                {
                    return platform::isAllowedBackgroundMessage(record.message);
                }
            ),
        };
    }

    auto runPipeline(
        ResolvedTarget resolved,
        WgcCaptureSession session,
        DeliveryTarget delivery,
        Templates const& templates,
        LoopConfig const& config,
        JsonlLog& log
    ) -> Result<RunSummary>
    {
        auto machine = Machine{
            resolved,
            std::move(session),
            delivery
        };
        auto outcome  = runLoops(machine, templates, config, log);
        auto shutdown = shutdownMachine(machine, log, outcome);
        if (!outcome)
        {
            return std::unexpected{std::move(outcome).error()};
        }
        if (!shutdown)
        {
            return std::unexpected{std::move(shutdown).error()};
        }
        return *std::move(outcome);
    }
}
