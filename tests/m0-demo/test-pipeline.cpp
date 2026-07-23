#include "test-helpers.hpp"

#include <pipeline.hpp>

#include <controller/discovery.hpp>
#include <controller/input.hpp>
#include <controller/target.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>
#include <domain/error.hpp>
#include <domain/space.hpp>
#include <vision/sad.hpp>

#include <doctest/doctest.h>

#include <Windows.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uf::m0_demo
{
    namespace
    {
        [[nodiscard]]
        auto transform800By450() -> CoordinateTransform
        {
            auto const result = CoordinateTransform::create(
                Point<DesktopSpace>{0.0F, 0.0F},
                1600.0F,
                900.0F,
                800,
                450
            );
            REQUIRE(result.has_value());
            return *result;
        }
    }

    TEST_CASE("m0 match center offsets by half the template extent")
    {
        auto const center = hitCenterFrame(
            SadMatch{10, 20, 0},
            8,
            6
        );
        CHECK(center.x() == 14.0F);
        CHECK(center.y() == 23.0F);
    }

    TEST_CASE("m0 match acceptance normalizes score by template area")
    {
        auto const matched = [](uint64 score)
        {
            return std::optional<SadMatch>{SadMatch{1, 2, score}};
        };

        CHECK(acceptMatch(matched(500), 10, 10, 5).has_value());
        CHECK_FALSE(acceptMatch(matched(501), 10, 10, 5).has_value());
        CHECK(acceptMatch(matched(20), 2, 2, 5).has_value());
        CHECK_FALSE(acceptMatch(matched(21), 2, 2, 5).has_value());
        CHECK_FALSE(
            acceptMatch(std::nullopt, 10, 10, 5).has_value()
        );
    }

    TEST_CASE("m0 target revalidation requires an unchanged identity")
    {
        CHECK(
            requireUnchangedTarget(
                RevalidateOutcome::Unchanged
            ).has_value()
        );

        for (auto const outcome : std::array{
            RevalidateOutcome::GenerationBumped,
            RevalidateOutcome::InstanceUnconfirmed,
        })
        {
            auto const result = requireUnchangedTarget(outcome);
            REQUIRE_FALSE(result.has_value());
            test_m0_demo::requireErrorKind(
                result.error(),
                AutomationErrorKind::StaleObservation
            );
        }

        auto const lost = requireUnchangedTarget(RevalidateOutcome::Lost);
        REQUIRE_FALSE(lost.has_value());
        test_m0_demo::requireErrorKind(
            lost.error(),
            AutomationErrorKind::ControllerDisconnected
        );
    }

    TEST_CASE("m0 transition frames cannot predate click completion")
    {
        auto const before = MonotonicInstant::now();
        auto const tick = std::chrono::duration_cast<MonotonicInstant::Duration>(
            std::chrono::milliseconds{1}
        );
        auto const barrier = before.checkedAdd(tick);
        if (!barrier)
        {
            FAIL("the first monotonic instant addition overflowed");
            return;
        }
        auto const after = barrier->checkedAdd(tick);
        if (!after)
        {
            FAIL("the second monotonic instant addition overflowed");
            return;
        }

        CHECK_FALSE(frameIsCausal(before, *barrier));
        CHECK(frameIsCausal(*barrier, *barrier));
        CHECK(frameIsCausal(*after, *barrier));
    }

    TEST_CASE("m0 step failure does not mask a guard violation")
    {
        CHECK(
            combineLoopStatus(StepStatus::Failed, false)
            == LoopStatus::FailedAndGuardViolation
        );
        CHECK(
            combineLoopStatus(StepStatus::TimedOut, true)
            == LoopStatus::Failed
        );
        CHECK(
            combineLoopStatus(StepStatus::Done, false)
            == LoopStatus::GuardViolation
        );
    }

    TEST_CASE("m0 ROI outside the frame reports the named configuration flag")
    {
        auto const transform = transform800By450();
        auto const rejected = ensureRoiInFrame(
            transform,
            "home",
            Rect<FrameSpace>{700.0F, 0.0F, 200.0F, 40.0F}
        );
        REQUIRE_FALSE(rejected.has_value());
        test_m0_demo::requireErrorKind(
            rejected.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK(rejected.error().message().find("--home-roi") != std::string_view::npos);
        CHECK(rejected.error().message().find("800x450") != std::string_view::npos);

        auto const accepted = ensureRoiInFrame(
            transform,
            "result",
            Rect<FrameSpace>{10.0F, 20.0F, 100.0F, 50.0F}
        );
        CHECK(accepted.has_value());
    }

    TEST_CASE("m0 template larger than its pixel ROI is a configuration error")
    {
        auto const transform = transform800By450();
        auto const imageTemplate = Template{
            .m_label = "home",
            .m_gray = std::vector<std::byte>(std::size_t{101} * 40U),
            .m_width = 101,
            .m_height = 40,
            .m_roi = Rect<FrameSpace>{0.0F, 0.0F, 100.0F, 40.0F},
        };

        auto const result = ensureTemplateFitsRoi(transform, imageTemplate);
        REQUIRE_FALSE(result.has_value());
        test_m0_demo::requireErrorKind(
            result.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK(result.error().message().find("--home-roi") != std::string_view::npos);
        CHECK(result.error().message().find("101x40") != std::string_view::npos);
        CHECK(result.error().message().find("100x40") != std::string_view::npos);
    }

    TEST_CASE("m0 empty client area is target unavailable")
    {
        auto const emptySizes = std::array{
            ClientSize{0, 480},
            ClientSize{640, 0},
            ClientSize{0, 0},
        };
        for (auto const size : emptySizes)
        {
            auto const result = ensureClientAreaUsable(size);
            REQUIRE_FALSE(result.has_value());
            test_m0_demo::requireErrorKind(
                result.error(),
                AutomationErrorKind::TargetUnavailable
            );
        }
        CHECK(ensureClientAreaUsable(ClientSize{640, 480}).has_value());
    }

    TEST_CASE("m0 run summary passes only when complete and clean")
    {
        auto const summary = [](
            uint32 attempted,
            uint32 succeeded,
            uint32 guardViolations,
            bool stopped,
            bool auditClean
        )
        {
            return RunSummary{
                attempted,
                succeeded,
                guardViolations,
                stopped,
                auditClean,
            };
        };

        CHECK(summary(100, 100, 0, false, true).passed());
        CHECK_FALSE(summary(100, 100, 0, false, false).passed());
        CHECK_FALSE(summary(100, 99, 1, false, true).passed());
        CHECK_FALSE(summary(100, 99, 0, false, true).passed());
        CHECK_FALSE(summary(50, 50, 0, true, true).passed());
        CHECK_FALSE(summary(0, 0, 0, true, true).passed());
    }

    TEST_CASE("m0 audit summary flags off-target or disallowed messages")
    {
        auto const record = [](uintptr target, uint32 message)
        {
            return AuditRecord{
                .m_target = target,
                .m_message = message,
                .m_wParam = 0,
                .m_lParam = 0,
                .m_at = MonotonicInstant::now(),
            };
        };
        auto constexpr target = uintptr{0x1234};
        auto const clean = std::array{
            record(target, WM_MOUSEMOVE),
            record(target, WM_LBUTTONDOWN),
            record(target, WM_LBUTTONUP),
        };
        auto const cleanSummary = summarizeAudit(clean, target);
        CHECK(cleanSummary.m_delivered == 3U);
        CHECK(cleanSummary.isClean());

        auto const offTarget = std::array{
            record(target, WM_LBUTTONDOWN),
            record(0x9999, WM_LBUTTONUP),
        };
        CHECK_FALSE(summarizeAudit(offTarget, target).m_allToTarget);

        auto const disallowed = std::array{record(target, 0x0111)};
        CHECK_FALSE(summarizeAudit(disallowed, target).m_allAllowed);

        auto const empty = std::span<AuditRecord const>{};
        CHECK(summarizeAudit(empty, target).isClean());
    }

    TEST_CASE("m0 bogus process selector resolves to target unavailable")
    {
        auto const selector = buildSelector(
            SelectorArgs{
                .m_process = std::numeric_limits<uint32>::max(),
            }
        );
        auto const candidates = std::vector<TargetCandidate>{};
        auto const result = resolveTarget(candidates, selector);
        REQUIRE_FALSE(result.has_value());
        test_m0_demo::requireErrorKind(
            result.error(),
            AutomationErrorKind::TargetUnavailable
        );
    }

    TEST_CASE("m0 selector construction copies every optional filter")
    {
        auto const selector = buildSelector(
            SelectorArgs{
                .m_process = 42,
                .m_windowHandle = 0x1234,
                .m_windowClass = "class",
                .m_title = "title",
            }
        );
        CHECK(selector.process() == std::optional{ProcessId{42}});
        CHECK(selector.windowHandle() == std::optional{WindowHandle{0x1234}});
        CHECK(selector.windowClass() == std::optional<std::string>{"class"});
        CHECK(selector.title() == std::optional<std::string>{"title"});
    }

}
