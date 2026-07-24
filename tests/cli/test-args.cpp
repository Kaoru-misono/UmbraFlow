#include <args.hpp>
#include <run.hpp>

#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::cli
{
    namespace
    {
        [[nodiscard]]
        auto parse(std::vector<std::string> const& raw) -> Result<RunArgs>
        {
            return parseRunArguments(raw);
        }

        template <typename Rep, typename Period>
        [[nodiscard]]
        auto asDuration(
            std::chrono::duration<Rep, Period> value
        ) -> MonotonicInstant::Duration
        {
            return std::chrono::duration_cast<MonotonicInstant::Duration>(value);
        }

        [[nodiscard]]
        auto minimalArgs() -> std::vector<std::string>
        {
            return {
                "--project",
                "proj",
                "--selector",
                "Game",
                "--page",
                "home",
                "--action",
                "start",
            };
        }

        [[nodiscard]]
        auto errorOfKind(AutomationErrorKind kind) -> Error
        {
            return fail(kind, "boundary test failure").error();
        }
    }

    TEST_CASE("parseRunArguments accepts every flag on the happy path")
    {
        auto const raw = std::vector<std::string>{
            "--project",             "proj",
            "--selector",            "Game Window",
            "--page",                "home",
            "--action",              "start",
            "--timeout",             "10",
            "--poll",                "100",
            "--budget",              "500",
            "--recognition-timeout", "1500",
            "--max-frame-age",       "600",
            "--trace",               "out.jsonl",
        };

        auto const result = parse(raw);
        REQUIRE(result.has_value());
        CHECK(result->m_project == "proj");
        CHECK(result->m_selector == "Game Window");
        CHECK(result->m_page == "home");
        CHECK(result->m_action == "start");
        CHECK(result->m_timeout == asDuration(std::chrono::seconds{10}));
        CHECK(result->m_pollInterval == asDuration(std::chrono::milliseconds{100}));
        CHECK(result->m_budget == uint64{500});
        CHECK(
            result->m_recognitionTimeout == asDuration(std::chrono::milliseconds{1500})
        );
        CHECK(result->m_maxFrameAge == asDuration(std::chrono::milliseconds{600}));
        CHECK(result->m_trace == "out.jsonl");
    }

    TEST_CASE("parseRunArguments applies defaults for omitted optional flags")
    {
        auto const result = parse(minimalArgs());
        REQUIRE(result.has_value());
        CHECK(result->m_timeout == k_defaultRunTimeout);
        CHECK(result->m_pollInterval == k_defaultRunPollInterval);
        CHECK(result->m_budget == k_defaultPixelComparisonBudget);
        CHECK(result->m_recognitionTimeout == k_defaultRunRecognitionTimeout);
        CHECK(result->m_maxFrameAge == k_defaultRunMaxFrameAge);
        CHECK(result->m_trace == k_defaultTracePath);
    }

    TEST_CASE("parseRunArguments reports each missing required flag")
    {
        struct Case final
        {
            std::string_view m_omitted;
            std::string_view m_expected;
        };

        auto const cases = std::vector<Case>{
            {"--project", "missing required argument --project"},
            {"--selector", "missing required argument --selector"},
            {"--page", "missing required argument --page"},
            {"--action", "missing required argument --action"},
        };

        for (auto const& testCase : cases)
        {
            auto raw   = std::vector<std::string>{};
            auto const full = minimalArgs();
            for (auto index = std::size_t{0}; index < full.size(); index += 2U)
            {
                if (full[index] == testCase.m_omitted)
                {
                    continue;
                }
                raw.emplace_back(full[index]);
                raw.emplace_back(full[index + 1U]);
            }

            auto const result = parse(raw);
            REQUIRE_FALSE(result.has_value());
            CHECK(automationErrorKind(result.error()) == AutomationErrorKind::InvalidResource);
            CHECK(result.error().message() == testCase.m_expected);
        }
    }

    TEST_CASE("parseRunArguments rejects an unknown flag")
    {
        auto raw = minimalArgs();
        raw.emplace_back("--bogus");
        raw.emplace_back("value");

        auto const result = parse(raw);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message() == "unknown argument \"--bogus\"");
    }

    TEST_CASE("parseRunArguments rejects a flag missing its value")
    {
        auto const raw = std::vector<std::string>{"--project"};

        auto const result = parse(raw);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message() == "missing value for --project");
    }

    TEST_CASE("parseRunArguments rejects a non-integer duration")
    {
        auto raw = minimalArgs();
        raw.emplace_back("--timeout");
        raw.emplace_back("soon");

        auto const result = parse(raw);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message() == "--timeout expects an integer, got \"soon\"");
    }

    TEST_CASE("parseRunArguments rejects a non-integer budget")
    {
        auto raw = minimalArgs();
        raw.emplace_back("--budget");
        raw.emplace_back("lots");

        auto const result = parse(raw);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message() == "--budget expects an integer, got \"lots\"");
    }

    TEST_CASE("parseRunArguments rejects a zero poll interval")
    {
        auto raw = minimalArgs();
        raw.emplace_back("--poll");
        raw.emplace_back("0");

        auto const result = parse(raw);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message() == "--poll must be between 1 and 60000 ms, got 0");
    }

    TEST_CASE("parseRunArguments rejects a poll interval above the maximum")
    {
        auto raw = minimalArgs();
        raw.emplace_back("--poll");
        raw.emplace_back("60001");

        auto const result = parse(raw);
        REQUIRE_FALSE(result.has_value());
        CHECK(
            result.error().message() == "--poll must be between 1 and 60000 ms, got 60001"
        );
    }

    TEST_CASE("parseRunArguments accepts the poll interval boundary values")
    {
        struct Case final
        {
            std::string_view           m_value;
            MonotonicInstant::Duration m_expected;
        };

        auto const cases = std::vector<Case>{
            {"1", asDuration(std::chrono::milliseconds{1})},
            {"60000", asDuration(std::chrono::milliseconds{60'000})},
        };

        for (auto const& testCase : cases)
        {
            auto raw = minimalArgs();
            raw.emplace_back("--poll");
            raw.emplace_back(testCase.m_value);

            auto const result = parse(raw);
            REQUIRE(result.has_value());
            CHECK(result->m_pollInterval == testCase.m_expected);
        }
    }

    TEST_CASE("ExitCode preserves the documented process values")
    {
        auto constexpr cases = std::array{
            std::pair{ExitCode::Success, uint8{0}},
            std::pair{ExitCode::Failure, uint8{1}},
            std::pair{ExitCode::TargetCompatibilityUnverified, uint8{2}},
            std::pair{ExitCode::ActionAbsent, uint8{3}},
            std::pair{ExitCode::Timeout, uint8{4}},
            std::pair{ExitCode::Cancelled, uint8{5}},
        };

        for (auto const& [code, value] : cases)
        {
            CHECK(std::to_underlying(code) == value);
        }
    }

    TEST_CASE("exitCodeForError maps each failure kind to its documented code")
    {
        auto constexpr cases = std::array{
            std::pair{AutomationErrorKind::Cancelled, ExitCode::Cancelled},
            std::pair{AutomationErrorKind::Timeout, ExitCode::Timeout},
            std::pair{
                AutomationErrorKind::TargetCompatibilityUnverified,
                ExitCode::TargetCompatibilityUnverified,
            },
            std::pair{AutomationErrorKind::CaptureStalled, ExitCode::Failure},
        };

        for (auto const& [kind, code] : cases)
        {
            CHECK(exitCodeForError(errorOfKind(kind), false) == code);
        }
    }

    TEST_CASE("exitCodeForError reports cancellation when a stop was requested")
    {
        // A Ctrl-C during a blocked capture surfaces as the capture failure, but
        // the operator's stop intent takes precedence in the reported exit code.
        auto constexpr kinds = std::array{
            AutomationErrorKind::CaptureStalled,
            AutomationErrorKind::Timeout,
            AutomationErrorKind::IoFailure,
        };

        for (auto const kind : kinds)
        {
            CHECK(exitCodeForError(errorOfKind(kind), true) == ExitCode::Cancelled);
        }
    }
}
