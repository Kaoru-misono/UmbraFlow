#include "test-helpers.hpp"

#include <ffi/png-decoder.hpp>
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
#include <utility>
#include <vector>

namespace uf::m0_demo
{
    namespace
    {
        [[nodiscard]]
        constexpr auto asByte(uint8 value) noexcept -> std::byte
        {
            return static_cast<std::byte>(value);
        }

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

    TEST_CASE("m0 RGBA to BGRA swaps red and blue")
    {
        auto rgba = std::vector<std::byte>{
            asByte(10),
            asByte(20),
            asByte(30),
            asByte(40),
            asByte(50),
            asByte(60),
            asByte(70),
            asByte(80),
        };
        auto const expected = std::vector<std::byte>{
            asByte(30),
            asByte(20),
            asByte(10),
            asByte(40),
            asByte(70),
            asByte(60),
            asByte(50),
            asByte(80),
        };
        CHECK(rgbaToBgra(std::move(rgba)) == expected);
    }

    TEST_CASE("m0 template grayscale uses the frame grayscale kernel")
    {
        auto rgba = std::vector<std::byte>{
            asByte(200),
            asByte(89),
            asByte(17),
            asByte(255),
        };
        auto const nativeBgra = std::array{
            asByte(17),
            asByte(89),
            asByte(200),
            asByte(255),
        };
        auto const viaFrame = bgra8ToGray8(nativeBgra, 1, 1, 4);
        auto const bgra = rgbaToBgra(std::move(rgba));
        auto const viaTemplate = bgra8ToGray8(bgra, 1, 1, 4);
        REQUIRE(viaFrame.has_value());
        REQUIRE(viaTemplate.has_value());
        CHECK(*viaFrame == *viaTemplate);
    }

    TEST_CASE("m0 BGRA crop honors stride and packs tightly")
    {
        auto constexpr width = std::size_t{3};
        auto constexpr stride = width * 4U + 8U;
        auto source = std::vector<std::byte>(stride * 3U, asByte(0xEE));
        for (auto y = std::size_t{0}; y < 3U; ++y)
        {
            for (auto x = std::size_t{0}; x < 3U; ++x)
            {
                auto const offset = y * stride + x * 4U;
                source.at(offset) = asByte(static_cast<uint8>(x));
                source.at(offset + 1U) = asByte(static_cast<uint8>(y));
                source.at(offset + 2U) = asByte(0);
                source.at(offset + 3U) = asByte(255);
            }
        }

        auto const rect = PixelRect::create(1, 1, 2, 2);
        REQUIRE(rect.has_value());
        auto const cropped = cropBgra(source, stride, *rect);
        REQUIRE(cropped.has_value());
        auto const expected = std::vector<std::byte>{
            asByte(1), asByte(1), asByte(0), asByte(255),
            asByte(2), asByte(1), asByte(0), asByte(255),
            asByte(1), asByte(2), asByte(0), asByte(255),
            asByte(2), asByte(2), asByte(0), asByte(255),
        };
        CHECK(*cropped == expected);
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

    TEST_CASE("m0 PNG decoder fails closed for malformed and oversized resources")
    {
        auto const empty = std::vector<std::byte>{};
        auto const malformed = ffi::decodePng(empty, "empty.png");
        REQUIRE_FALSE(malformed.has_value());
        test_m0_demo::requireErrorKind(
            malformed.error(),
            AutomationErrorKind::InvalidResource
        );

        auto const oversizedHeader = std::array{
            asByte(0x89), asByte(0x50), asByte(0x4E), asByte(0x47),
            asByte(0x0D), asByte(0x0A), asByte(0x1A), asByte(0x0A),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x0D),
            asByte(0x49), asByte(0x48), asByte(0x44), asByte(0x52),
            asByte(0x00), asByte(0x00), asByte(0x20), asByte(0x01),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x01),
            asByte(0x08), asByte(0x06), asByte(0x00), asByte(0x00),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x00),
            asByte(0x00),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x00),
            asByte(0x49), asByte(0x44), asByte(0x41), asByte(0x54),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x00),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x00),
            asByte(0x49), asByte(0x45), asByte(0x4E), asByte(0x44),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x00),
        };
        auto const oversized = ffi::decodePng(
            oversizedHeader,
            "dimension-limit.png"
        );
        REQUIRE_FALSE(oversized.has_value());
        test_m0_demo::requireErrorKind(
            oversized.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK(
            oversized.error().message().find("8193x1 exceeds 8192 pixels per axis")
            != std::string_view::npos
        );
    }

    TEST_CASE("m0 PNG decoder rejects non-PNG and truncated chunk lengths")
    {
        auto const jpeg = std::array{
            asByte(0xFF),
            asByte(0xD8),
            asByte(0xFF),
            asByte(0xE0),
        };
        auto const nonPng = ffi::decodePng(jpeg, "template.jpg");
        REQUIRE_FALSE(nonPng.has_value());
        test_m0_demo::requireErrorKind(
            nonPng.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK(nonPng.error().message().find("not a PNG") != std::string_view::npos);

        auto const truncatedChunk = std::array{
            asByte(0x89), asByte(0x50), asByte(0x4E), asByte(0x47),
            asByte(0x0D), asByte(0x0A), asByte(0x1A), asByte(0x0A),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x0D),
            asByte(0x49), asByte(0x48), asByte(0x44), asByte(0x52),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x01),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x01),
            asByte(0x08), asByte(0x06), asByte(0x00), asByte(0x00),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x00),
            asByte(0x00),
            asByte(0x7F), asByte(0xFF), asByte(0xFF), asByte(0xFF),
            asByte(0x74), asByte(0x45), asByte(0x58), asByte(0x74),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x00),
        };
        auto const truncated = ffi::decodePng(
            truncatedChunk,
            "truncated-chunk.png"
        );
        REQUIRE_FALSE(truncated.has_value());
        test_m0_demo::requireErrorKind(
            truncated.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK(
            truncated.error().message().find("declared chunk length exceeds the input")
            != std::string_view::npos
        );
    }

    TEST_CASE("m0 16-bit PNG downconversion uses round-to-nearest")
    {
        auto const encoded = std::array{
            asByte(0x89), asByte(0x50), asByte(0x4E), asByte(0x47),
            asByte(0x0D), asByte(0x0A), asByte(0x1A), asByte(0x0A),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x0D),
            asByte(0x49), asByte(0x48), asByte(0x44), asByte(0x52),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x01),
            asByte(0x00), asByte(0x00), asByte(0x00), asByte(0x01),
            asByte(0x10), asByte(0x06), asByte(0x00), asByte(0x00),
            asByte(0x00), asByte(0x4F), asByte(0x85), asByte(0x18),
            asByte(0xCA), asByte(0x00), asByte(0x00), asByte(0x00),
            asByte(0x11), asByte(0x49), asByte(0x44), asByte(0x41),
            asByte(0x54), asByte(0x78), asByte(0x9C), asByte(0x63),
            asByte(0x60), asByte(0x68), asByte(0x6C), asByte(0x68),
            asByte(0xF8), asByte(0xFF), asByte(0xBF), asByte(0x81),
            asByte(0x01), asByte(0x00), asByte(0x11), asByte(0x09),
            asByte(0x04), asByte(0x00), asByte(0x81), asByte(0xEE),
            asByte(0x58), asByte(0x57), asByte(0x00), asByte(0x00),
            asByte(0x00), asByte(0x00), asByte(0x49), asByte(0x45),
            asByte(0x4E), asByte(0x44), asByte(0xAE), asByte(0x42),
            asByte(0x60), asByte(0x82),
        };
        auto const decoded = ffi::decodePng(encoded, "rgba16.png");
        REQUIRE(decoded.has_value());
        CHECK(decoded->m_width == 1U);
        CHECK(decoded->m_height == 1U);
        auto const expected = std::vector{
            asByte(1),
            asByte(128),
            asByte(255),
            asByte(128),
        };
        CHECK(decoded->m_pixels == expected);
    }
}
