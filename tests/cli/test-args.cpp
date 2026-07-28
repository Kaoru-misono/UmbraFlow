#include <args.hpp>
#include <run.hpp>

#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <source_location>
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

    TEST_CASE("run error rendering includes its originating source location")
    {
        auto const location = std::source_location::current();
        auto failure = fail(
            AutomationErrorKind::InvalidResource,
            "bad project",
            {},
            location
        );

        auto const rendered = formatRunError(failure.error());
        auto origin = std::filesystem::path{location.file_name()}
                          .filename()
                          .string();
        origin += ':';
        origin += std::to_string(location.line());
        CHECK(rendered.find(origin) != std::string::npos);
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
        CHECK(result->project == "proj");
        CHECK(result->selector == "Game Window");
        CHECK(result->page == "home");
        CHECK(result->action == "start");
        CHECK(result->timeout == asDuration(std::chrono::seconds{10}));
        CHECK(result->pollInterval == asDuration(std::chrono::milliseconds{100}));
        CHECK(result->budget == uint64{500});
        CHECK(
            result->recognitionTimeout == asDuration(std::chrono::milliseconds{1500})
        );
        CHECK(result->maxFrameAge == asDuration(std::chrono::milliseconds{600}));
        CHECK(result->trace == "out.jsonl");
    }

    TEST_CASE("parseRunArguments accepts the script path with --task")
    {
        auto const raw = std::vector<std::string>{
            "--project",  "proj",
            "--selector", "Game",
            "--task",     "daily",
        };

        auto const result = parse(raw);
        REQUIRE(result.has_value());
        CHECK(result->task == "daily");
        CHECK(result->page.empty());
        CHECK(result->action.empty());
        CHECK(result->project == "proj");
        CHECK(result->selector == "Game");
    }

    TEST_CASE("parseRunArguments rejects --task combined with the smoke-path flags")
    {
        auto constexpr expected =
            "--task cannot be combined with --page or --action";

        SUBCASE("with --page")
        {
            auto const raw = std::vector<std::string>{
                "--project", "proj", "--selector", "Game",
                "--task",    "daily", "--page",     "home",
            };
            auto const result = parse(raw);
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().message() == expected);
        }
        SUBCASE("with --action")
        {
            auto const raw = std::vector<std::string>{
                "--project", "proj",  "--selector", "Game",
                "--task",    "daily", "--action",   "start",
            };
            auto const result = parse(raw);
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().message() == expected);
        }
        SUBCASE("with both page and action")
        {
            auto raw = minimalArgs();
            raw.emplace_back("--task");
            raw.emplace_back("daily");
            auto const result = parse(raw);
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().message() == expected);
        }
    }

    TEST_CASE("runUsageText documents the --task run mode")
    {
        auto const usage = runUsageText();
        CHECK(usage.find("--task") != std::string_view::npos);
    }

    TEST_CASE("parseRunArguments applies defaults for omitted optional flags")
    {
        auto const result = parse(minimalArgs());
        REQUIRE(result.has_value());
        CHECK(result->timeout == k_defaultRunTimeout);
        CHECK(result->pollInterval == k_defaultRunPollInterval);
        CHECK(result->budget == k_defaultPixelComparisonBudget);
        CHECK(result->recognitionTimeout == k_defaultRunRecognitionTimeout);
        CHECK(result->maxFrameAge == k_defaultRunMaxFrameAge);
        CHECK(result->trace == k_defaultTracePath);
    }

    TEST_CASE("parseRunArguments reports each missing required flag")
    {
        struct Case final
        {
            std::string_view omitted;
            std::string_view expected;
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
                if (full[index] == testCase.omitted)
                {
                    continue;
                }
                raw.emplace_back(full[index]);
                raw.emplace_back(full[index + 1U]);
            }

            auto const result = parse(raw);
            REQUIRE_FALSE(result.has_value());
            CHECK(automationErrorKind(result.error()) == AutomationErrorKind::InvalidResource);
            CHECK(result.error().message() == testCase.expected);
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
            std::string_view           value;
            MonotonicInstant::Duration expected;
        };

        auto const cases = std::vector<Case>{
            {"1", asDuration(std::chrono::milliseconds{1})},
            {"60000", asDuration(std::chrono::milliseconds{60'000})},
        };

        for (auto const& testCase : cases)
        {
            auto raw = minimalArgs();
            raw.emplace_back("--poll");
            raw.emplace_back(testCase.value);

            auto const result = parse(raw);
            REQUIRE(result.has_value());
            CHECK(result->pollInterval == testCase.expected);
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
