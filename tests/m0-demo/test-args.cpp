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
        CHECK(result->m_selector.m_process == std::optional<uint32>{1234});
        CHECK(result->m_homeTemplate == std::filesystem::path{"home.png"});
        auto const expectedHomeRoi = Rect<FrameSpace>{0.0F, 0.0F, 100.0F, 40.0F};
        auto const expectedResultRoi = Rect<FrameSpace>{10.0F, 20.0F, 50.0F, 50.0F};
        auto const expectedResetRoi = Rect<FrameSpace>{5.0F, 6.0F, 7.0F, 8.0F};
        CHECK(result->m_homeRoi == expectedHomeRoi);
        CHECK(result->m_resultRoi == expectedResultRoi);
        CHECK(result->m_resetRoi == expectedResetRoi);
        CHECK(result->m_threshold == 25U);
        CHECK(result->m_mode == Mode::Guard);
        CHECK(result->m_loops == 1U);
        CHECK(
            result->m_maxActionFrameAge
            == std::chrono::duration_cast<MonotonicInstant::Duration>(
                std::chrono::milliseconds{750}
            )
        );
        CHECK(
            result->m_stallTimeout
            == std::chrono::duration_cast<MonotonicInstant::Duration>(
                std::chrono::milliseconds{1000}
            )
        );
        CHECK_FALSE(result->m_log.has_value());
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
        CHECK(result->m_selector.m_windowHandle == std::optional<intptr>{0x1A2B});
        CHECK(result->m_mode == Mode::Coexist);
        CHECK(result->m_loops == 100U);
        CHECK(
            result->m_maxActionFrameAge
            == std::chrono::duration_cast<MonotonicInstant::Duration>(
                std::chrono::milliseconds{300}
            )
        );
        CHECK(
            result->m_stallTimeout
            == std::chrono::duration_cast<MonotonicInstant::Duration>(
                std::chrono::milliseconds{500}
            )
        );
        CHECK(result->m_log == std::optional<std::filesystem::path>{"run.jsonl"});
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
        CHECK(accepted->m_loops == 1U);
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
        if (!ranged || !ranged->m_clickDelay)
        {
            FAIL("the ranged click delay did not parse");
            return;
        }
        CHECK(ranged->m_clickDelay->minimumMilliseconds() == 600U);
        CHECK(ranged->m_clickDelay->maximumMilliseconds() == 1800U);
        CHECK(ranged->m_seed == 12345U);

        auto fixedRaw = fullArguments();
        append(fixedRaw, {"--click-delay-ms", "1000"});
        auto const fixed = parseArguments(fixedRaw);
        if (!fixed || !fixed->m_clickDelay)
        {
            FAIL("the fixed click delay did not parse");
            return;
        }
        CHECK(fixed->m_clickDelay->minimumMilliseconds() == 1000U);
        CHECK(fixed->m_clickDelay->maximumMilliseconds() == 1000U);
    }

    TEST_CASE("m0 click delay defaults to none and seed to the fixed constant")
    {
        auto const result = parseArguments(fullArguments());
        REQUIRE(result.has_value());
        CHECK_FALSE(result->m_clickDelay.has_value());
        CHECK(result->m_seed == g_defaultPacingSeed);
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
        CHECK(result->m_selector.m_title == std::optional<std::string>{"Capture Target"});
        CHECK(result->m_output == std::filesystem::path{"capture.png"});
        CHECK(result->m_frames == g_defaultCaptureFrames);
        CHECK(result->m_interval == g_defaultCaptureInterval);
        CHECK_FALSE(result->m_log.has_value());

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
        CHECK(result->m_selector.m_process == std::optional<uint32>{20});
        CHECK(result->m_selector.m_windowHandle == std::optional<intptr>{0x1A2B});
        CHECK(result->m_selector.m_title == std::optional<std::string>{"new"});
        CHECK(result->m_output == std::filesystem::path{"new.png"});
        CHECK(result->m_frames == 3U);
        CHECK(
            result->m_interval
            == std::chrono::duration_cast<MonotonicInstant::Duration>(
                std::chrono::milliseconds{25}
            )
        );
        CHECK(result->m_log == std::optional<std::filesystem::path>{"capture.jsonl"});
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
        CHECK(boundary->m_frames == std::numeric_limits<uint32>::max());
        CHECK(boundary->m_interval == MonotonicInstant::Duration::zero());
    }

    TEST_CASE("m0 input-agent arguments require file IPC paths and apply defaults")
    {
        auto const result = parseInputAgentArguments(
            argumentsOf(
                {
                    "--hwnd",
                    "0x1A2B",
                    "--queue",
                    "commands.jsonl",
                    "--results",
                    "results.jsonl",
                    "--output-dir",
                    "agent-output",
                }
            )
        );

        REQUIRE(result.has_value());
        CHECK(result->m_windowHandle == intptr{0x1A2B});
        CHECK(result->m_queue == std::filesystem::path{"commands.jsonl"});
        CHECK(result->m_results == std::filesystem::path{"results.jsonl"});
        CHECK(result->m_outputDirectory == std::filesystem::path{"agent-output"});
        CHECK(result->m_idleTimeout == g_defaultInputAgentIdleTimeout);
    }

    TEST_CASE("m0 input-agent arguments reject missing and invalid values")
    {
        auto const cases = std::vector<std::vector<std::string>>{
            argumentsOf({}),
            argumentsOf(
                {
                    "--queue",
                    "commands.jsonl",
                    "--results",
                    "results.jsonl",
                    "--output-dir",
                    "agent-output",
                }
            ),
            argumentsOf(
                {
                    "--hwnd",
                    "0x1",
                    "--queue",
                    "commands.jsonl",
                    "--results",
                    "results.jsonl",
                }
            ),
            argumentsOf(
                {
                    "--hwnd",
                    "0x1",
                    "--queue",
                    "commands.jsonl",
                    "--results",
                    "results.jsonl",
                    "--output-dir",
                    "",
                }
            ),
            argumentsOf(
                {
                    "--hwnd",
                    "0x1",
                    "--queue",
                    "",
                    "--results",
                    "results.jsonl",
                    "--output-dir",
                    "agent-output",
                }
            ),
            argumentsOf(
                {
                    "--hwnd",
                    "0x1",
                    "--queue",
                    "commands.jsonl",
                    "--results",
                    "results.jsonl",
                    "--output-dir",
                    "agent-output",
                    "--idle-timeout-s",
                    "0",
                }
            ),
            argumentsOf(
                {
                    "--hwnd",
                    "0x1",
                    "--queue",
                    "commands.jsonl",
                    "--results",
                    "results.jsonl",
                    "--output-dir",
                    "agent-output",
                    "--pid",
                    "1",
                }
            ),
        };

        for (auto const& raw : cases)
        {
            auto const result = parseInputAgentArguments(raw);
            REQUIRE_FALSE(result.has_value());
            test_m0_demo::requireErrorKind(
                result.error(),
                AutomationErrorKind::InvalidResource
            );
        }
    }
}
