#include "test-helpers.hpp"

#include <args.hpp>
#include <pacing.hpp>

#include <core/types/integer.hpp>
#include <domain/error.hpp>
#include <domain/space.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <optional>
#include <ranges>
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
        auto argumentsOf(
            std::initializer_list<std::string_view> parts
        ) -> std::vector<std::string>
        {
            auto arguments = std::vector<std::string>{};
            arguments.reserve(parts.size());
            for (auto const part : parts)
            {
                arguments.emplace_back(part);
            }
            return arguments;
        }

        [[nodiscard]]
        auto fullArguments() -> std::vector<std::string>
        {
            return argumentsOf(
                {
                    "--pid",
                    "1234",
                    "--home-template",
                    "home.png",
                    "--home-roi",
                    "0,0,100,40",
                    "--result-template",
                    "result.png",
                    "--result-roi",
                    "10,20,50,50",
                    "--reset-template",
                    "reset.png",
                    "--reset-roi",
                    "5,6,7,8",
                    "--threshold",
                    "25",
                }
            );
        }

        auto append(
            std::vector<std::string>& destination,
            std::initializer_list<std::string_view> values
        ) -> void
        {
            for (auto const value : values)
            {
                destination.emplace_back(value);
            }
        }
    }

    TEST_CASE("m0 arguments parse a complete set with defaults")
    {
        auto const result = parseArguments(fullArguments());
        REQUIRE(result.has_value());
        CHECK(result->selector.process == std::optional<uint32>{1234});
        CHECK(result->homeTemplate == std::filesystem::path{"home.png"});
        auto const expectedHomeRoi = Rect<FrameSpace>{0.0F, 0.0F, 100.0F, 40.0F};
        auto const expectedResultRoi = Rect<FrameSpace>{10.0F, 20.0F, 50.0F, 50.0F};
        auto const expectedResetRoi = Rect<FrameSpace>{5.0F, 6.0F, 7.0F, 8.0F};
        CHECK(result->homeRoi == expectedHomeRoi);
        CHECK(result->resultRoi == expectedResultRoi);
        CHECK(result->resetRoi == expectedResetRoi);
        CHECK(result->threshold == 25U);
        CHECK(result->mode == Mode::Guard);
        CHECK(result->loops == 1U);
        CHECK(
            result->maxActionFrameAge
            == std::chrono::duration_cast<MonotonicInstant::Duration>(
                std::chrono::milliseconds{750}
            )
        );
        CHECK(
            result->stallTimeout
            == std::chrono::duration_cast<MonotonicInstant::Duration>(
                std::chrono::milliseconds{1000}
            )
        );
        CHECK_FALSE(result->log.has_value());
    }

    TEST_CASE("m0 arguments override defaults and parse a hexadecimal window handle")
    {
        auto raw = fullArguments();
        append(
            raw,
            {
                "--hwnd",
                "0x1A2B",
                "--mode",
                "coexist",
                "--loops",
                "100",
                "--max-action-frame-age",
                "300",
                "--stall-timeout",
                "500",
                "--log",
                "run.jsonl",
            }
        );

        auto const result = parseArguments(raw);
        REQUIRE(result.has_value());
        CHECK(result->selector.windowHandle == std::optional<intptr>{0x1A2B});
        CHECK(result->mode == Mode::Coexist);
        CHECK(result->loops == 100U);
        CHECK(
            result->maxActionFrameAge
            == std::chrono::duration_cast<MonotonicInstant::Duration>(
                std::chrono::milliseconds{300}
            )
        );
        CHECK(
            result->stallTimeout
            == std::chrono::duration_cast<MonotonicInstant::Duration>(
                std::chrono::milliseconds{500}
            )
        );
        CHECK(result->log == std::optional<std::filesystem::path>{"run.jsonl"});
    }

    TEST_CASE("m0 arguments reject zero loops")
    {
        auto raw = fullArguments();
        append(raw, {"--loops", "0"});
        auto const rejected = parseArguments(raw);
        REQUIRE_FALSE(rejected.has_value());
        test_m0_demo::requireErrorKind(
            rejected.error(),
            AutomationErrorKind::InvalidResource
        );

        auto acceptedRaw = fullArguments();
        append(acceptedRaw, {"--loops", "1"});
        auto const accepted = parseArguments(acceptedRaw);
        REQUIRE(accepted.has_value());
        CHECK(accepted->loops == 1U);
    }

    TEST_CASE("m0 arguments fail closed for missing or malformed input")
    {
        auto wrongThreshold = fullArguments();
        auto const threshold = std::ranges::find(wrongThreshold, "25");
        REQUIRE(threshold != wrongThreshold.end());
        *threshold = "abc";

        auto unknown = fullArguments();
        append(unknown, {"--nope", "1"});

        auto cases = std::vector<std::vector<std::string>>{};
        cases.emplace_back(argumentsOf({"--pid", "1"}));
        cases.emplace_back(
            argumentsOf(
                {
                    "--home-template",
                    "h.png",
                    "--home-roi",
                    "0,0,100",
                    "--result-template",
                    "r.png",
                    "--result-roi",
                    "0,0,1,1",
                    "--reset-template",
                    "x.png",
                    "--reset-roi",
                    "0,0,1,1",
                    "--threshold",
                    "1",
                }
            )
        );
        cases.emplace_back(std::move(wrongThreshold));
        cases.emplace_back(argumentsOf({"--threshold"}));
        cases.emplace_back(argumentsOf({"--help"}));
        cases.emplace_back(std::move(unknown));

        for (auto const& raw : cases)
        {
            auto const result = parseArguments(raw);
            REQUIRE_FALSE(result.has_value());
            test_m0_demo::requireErrorKind(
                result.error(),
                AutomationErrorKind::InvalidResource
            );
        }
    }

    TEST_CASE("m0 arguments parse fixed and ranged click delays with a seed")
    {
        auto rangedRaw = fullArguments();
        append(rangedRaw, {"--click-delay-ms", "600-1800", "--seed", "12345"});
        auto const ranged = parseArguments(rangedRaw);
        if (!ranged || !ranged->clickDelay)
        {
            FAIL("the ranged click delay did not parse");
            return;
        }
        CHECK(ranged->clickDelay->minimumMilliseconds() == 600U);
        CHECK(ranged->clickDelay->maximumMilliseconds() == 1800U);
        CHECK(ranged->seed == 12345U);

        auto fixedRaw = fullArguments();
        append(fixedRaw, {"--click-delay-ms", "1000"});
        auto const fixed = parseArguments(fixedRaw);
        if (!fixed || !fixed->clickDelay)
        {
            FAIL("the fixed click delay did not parse");
            return;
        }
        CHECK(fixed->clickDelay->minimumMilliseconds() == 1000U);
        CHECK(fixed->clickDelay->maximumMilliseconds() == 1000U);
    }

    TEST_CASE("m0 click delay defaults to none and seed to the fixed constant")
    {
        auto const result = parseArguments(fullArguments());
        REQUIRE(result.has_value());
        CHECK_FALSE(result->clickDelay.has_value());
        CHECK(result->seed == k_defaultPacingSeed);
    }

    TEST_CASE("m0 arguments reject malformed click delay specifications")
    {
        auto const specifications = std::vector<std::string_view>{
            "0",
            "0-100",
            "100-0",
            "1800-600",
            "abc",
            "600-",
            "-600",
            "600-1800-2000",
            "",
        };
        for (auto const specification : specifications)
        {
            auto raw = fullArguments();
            append(raw, {"--click-delay-ms", specification});
            auto const result = parseArguments(raw);
            REQUIRE_FALSE(result.has_value());
            test_m0_demo::requireErrorKind(
                result.error(),
                AutomationErrorKind::InvalidResource
            );
        }
    }

    TEST_CASE("m0 arguments reject a threshold above the grayscale range")
    {
        auto raw = fullArguments();
        auto const threshold = std::ranges::find(raw, "25");
        REQUIRE(threshold != raw.end());
        *threshold = "256";

        auto const result = parseArguments(raw);
        REQUIRE_FALSE(result.has_value());
        test_m0_demo::requireErrorKind(
            result.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK(result.error().message().find("0..=255") != std::string_view::npos);
    }

    TEST_CASE("m0 capture arguments require output and apply capture defaults")
    {
        auto const raw = argumentsOf(
            {
                "--title",
                "Capture Target",
                "--out",
                "capture.png",
            }
        );
        auto const result = parseCaptureArguments(raw);

        REQUIRE(result.has_value());
        CHECK(result->selector.title == std::optional<std::string>{"Capture Target"});
        CHECK(result->output == std::filesystem::path{"capture.png"});
        CHECK(result->frames == k_defaultCaptureFrames);
        CHECK(result->interval == k_defaultCaptureInterval);
        CHECK_FALSE(result->log.has_value());

        auto const missingOutput = parseCaptureArguments(
            std::span<std::string const>{}
        );
        REQUIRE_FALSE(missingOutput.has_value());
        test_m0_demo::requireErrorKind(
            missingOutput.error(),
            AutomationErrorKind::InvalidResource
        );
    }

    TEST_CASE("m0 capture arguments reuse selectors and keep the last duplicate")
    {
        auto const raw = argumentsOf(
            {
                "--pid",
                "10",
                "--pid",
                "20",
                "--hwnd",
                "12",
                "--hwnd",
                "0x1A2B",
                "--title",
                "old",
                "--title",
                "new",
                "--out",
                "old.png",
                "--out",
                "new.png",
                "--frames",
                "2",
                "--frames",
                "3",
                "--interval-ms",
                "10",
                "--interval-ms",
                "25",
                "--log",
                "capture.jsonl",
            }
        );
        auto const result = parseCaptureArguments(raw);

        REQUIRE(result.has_value());
        CHECK(result->selector.process == std::optional<uint32>{20});
        CHECK(result->selector.windowHandle == std::optional<intptr>{0x1A2B});
        CHECK(result->selector.title == std::optional<std::string>{"new"});
        CHECK(result->output == std::filesystem::path{"new.png"});
        CHECK(result->frames == 3U);
        CHECK(
            result->interval
            == std::chrono::duration_cast<MonotonicInstant::Duration>(
                std::chrono::milliseconds{25}
            )
        );
        CHECK(result->log == std::optional<std::filesystem::path>{"capture.jsonl"});
    }

    TEST_CASE("m0 capture arguments fail closed at selector and numeric bounds")
    {
        auto const cases = std::vector<std::vector<std::string>>{
            argumentsOf({"--out", "capture.png", "--pid", "not-a-pid"}),
            argumentsOf({"--out", "capture.png", "--hwnd", "0xnot-a-handle"}),
            argumentsOf({"--out", "capture.png", "--class", "NotAllowed"}),
            argumentsOf({"--out", "capture.png", "--frames", "0"}),
            argumentsOf({"--out", "capture.png", "--frames", "4294967296"}),
            argumentsOf({"--out", "capture.png", "--interval-ms", "-1"}),
            argumentsOf(
                {
                    "--out",
                    "capture.png",
                    "--interval-ms",
                    "18446744073709551615",
                }
            ),
            argumentsOf({"--out", ""}),
            argumentsOf({"--out"}),
        };

        for (auto const& raw : cases)
        {
            auto const result = parseCaptureArguments(raw);
            REQUIRE_FALSE(result.has_value());
            test_m0_demo::requireErrorKind(
                result.error(),
                AutomationErrorKind::InvalidResource
            );
        }

        auto const boundary = parseCaptureArguments(
            argumentsOf(
                {
                    "--out",
                    "capture.png",
                    "--frames",
                    "4294967295",
                    "--interval-ms",
                    "0",
                }
            )
        );
        REQUIRE(boundary.has_value());
        CHECK(boundary->frames == std::numeric_limits<uint32>::max());
        CHECK(boundary->interval == MonotonicInstant::Duration::zero());
    }
}
