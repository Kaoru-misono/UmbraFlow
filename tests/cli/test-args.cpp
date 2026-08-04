#include <args.hpp>
#include <run.hpp>

#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <task/task-host.hpp>

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
                "--task",
                "daily",
            };
        }

        [[nodiscard]]
        auto errorOfKind(AutomationErrorKind kind) -> Error
        {
            return fail(kind, "boundary test failure").error();
        }

        [[nodiscard]]
        auto reportFailedWith(AutomationErrorKind kind) -> task::TaskRunReport
        {
            return task::TaskRunReport{
                .taskName = "daily",
                .failure  = errorOfKind(kind),
            };
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
            "--task",                "daily",
            "--budget",              "500",
            "--recognition-timeout", "1500",
            "--max-frame-age",       "600",
            "--max-runtime",         "900000",
            "--trace",               "out.jsonl",
        };

        auto const result = parse(raw);
        REQUIRE(result.has_value());
        CHECK(result->project == "proj");
        CHECK(result->selector == "Game Window");
        CHECK(result->task == "daily");
        CHECK(result->budget == uint64{500});
        CHECK(
            result->recognitionTimeout == asDuration(std::chrono::milliseconds{1500})
        );
        CHECK(result->maxFrameAge == asDuration(std::chrono::milliseconds{600}));
        CHECK(result->maxRuntime == asDuration(std::chrono::milliseconds{900000}));
        CHECK(result->trace == "out.jsonl");
    }

    TEST_CASE("parseRunArguments no longer accepts the removed run-shape flags")
    {
        // None of the four is merely ignored: an invocation carrying one is
        // refused by name, so a stale script fails loudly instead of quietly
        // running under a budget nobody applies.
        auto constexpr removed = std::array<std::string_view, 4>{
            "--page",
            "--action",
            "--timeout",
            "--poll",
        };

        for (auto const flag : removed)
        {
            auto raw = minimalArgs();
            raw.emplace_back(flag);
            raw.emplace_back("home");

            auto const result = parse(raw);
            REQUIRE_FALSE(result.has_value());
            CHECK(
                result.error().message()
                == std::string{"unknown argument \""} + std::string{flag} + "\""
            );
        }
    }

    TEST_CASE("runUsageText documents the --task run mode")
    {
        auto const usage = runUsageText();
        CHECK(usage.find("--task") != std::string_view::npos);
        CHECK(usage.find("--max-runtime") != std::string_view::npos);
        CHECK(usage.find("--page") == std::string_view::npos);
        CHECK(usage.find("--action") == std::string_view::npos);
    }

    TEST_CASE("parseRunArguments applies defaults for omitted optional flags")
    {
        auto const result = parse(minimalArgs());
        REQUIRE(result.has_value());
        CHECK(result->budget == k_defaultPixelComparisonBudget);
        CHECK(result->recognitionTimeout == k_defaultRunRecognitionTimeout);
        CHECK(result->maxFrameAge == k_defaultRunMaxFrameAge);
        // Resolved by the parser rather than by the composition: an omitted flag
        // leaves the field carrying the ceiling the run is actually held to.
        CHECK(result->maxRuntime == k_defaultRunMaxRuntime);
        CHECK(result->trace == k_defaultTracePath);
        // Absent by default: a run that never asked for --ocr-models must not
        // pay for an engine, and cycle_read refuses on its own terms instead.
        CHECK_FALSE(result->ocrModels.has_value());
    }

    TEST_CASE("parseRunArguments accepts --ocr-models and passes the directory through")
    {
        auto raw = minimalArgs();
        raw.emplace_back("--ocr-models");
        raw.emplace_back("models");

        auto const result = parse(raw);
        REQUIRE(result.has_value());
        REQUIRE(result->ocrModels.has_value());
        CHECK(*result->ocrModels == std::filesystem::path{"models"});
    }

    TEST_CASE("parseExploreArguments accepts the same optional --ocr-models flag as run")
    {
        auto const withoutFlag = std::vector<std::string>{
            "--project",  "proj",
            "--selector", "Game",
            "--queue",    "queue.jsonl",
            "--results",  "results.jsonl",
        };
        auto const withoutResult = parseExploreArguments(withoutFlag);
        REQUIRE(withoutResult.has_value());
        CHECK_FALSE(withoutResult->ocrModels.has_value());

        auto withFlag = withoutFlag;
        withFlag.emplace_back("--ocr-models");
        withFlag.emplace_back("models");

        auto const withResult = parseExploreArguments(withFlag);
        REQUIRE(withResult.has_value());
        REQUIRE(withResult->ocrModels.has_value());
        CHECK(*withResult->ocrModels == std::filesystem::path{"models"});
    }

    TEST_CASE("parseCheckArguments accepts the same optional --ocr-models flag")
    {
        // An element with no template identifies by the text its rectangle
        // reads, so the matrix needs the models directory: a parser that
        // accepts the flag and then drops it must go red on the read-back.
        auto const withoutFlag = std::vector<std::string>{"--project", "proj"};
        auto const withoutResult = parseCheckArguments(withoutFlag);
        REQUIRE(withoutResult.has_value());
        CHECK_FALSE(withoutResult->ocrModels.has_value());
        CHECK(withoutResult->trace == k_defaultCheckTracePath);

        auto withFlag = withoutFlag;
        withFlag.emplace_back("--ocr-models");
        withFlag.emplace_back("models");

        auto const withResult = parseCheckArguments(withFlag);
        REQUIRE(withResult.has_value());
        REQUIRE(withResult->ocrModels.has_value());
        CHECK(*withResult->ocrModels == std::filesystem::path{"models"});

        CHECK(checkUsageText().find("--ocr-models") != std::string_view::npos);
    }

    TEST_CASE("--sweep-pages is a switch, off unless it is named")
    {
        // The one flag here that takes no value, so the parser must not consume
        // the argument after it: read it as a value flag and the --trace below is
        // swallowed as this one's value and the path never arrives.
        auto const bare = std::vector<std::string>{"--project", "proj"};

        auto const without = parseCheckArguments(bare);
        REQUIRE(without.has_value());
        CHECK_FALSE(without->sweepPages);

        auto named = bare;
        named.emplace_back("--sweep-pages");
        named.emplace_back("--trace");
        named.emplace_back("out.jsonl");

        auto const with = parseCheckArguments(named);
        REQUIRE(with.has_value());
        CHECK(with->sweepPages);
        CHECK(with->trace == std::filesystem::path{"out.jsonl"});

        // A value handed to it is an argument nothing takes, and saying so is the
        // whole reason the switch is refused rather than quietly tolerated.
        auto valued = bare;
        valued.emplace_back("--sweep-pages");
        valued.emplace_back("true");
        CHECK_FALSE(parseCheckArguments(valued).has_value());

        CHECK(checkUsageText().find("--sweep-pages") != std::string_view::npos);
    }

    TEST_CASE("the default comparison budget covers a page evaluation but not a full frame")
    {
        // A candidate position costs the template's pixels; a search walks one
        // position per placement that still fits inside the region.
        auto constexpr searchCost = [](
            uint64 templateWidth,
            uint64 templateHeight,
            uint64 roiWidth,
            uint64 roiHeight
        ) -> uint64
        {
            return (roiWidth - templateWidth + 1U)
                * (roiHeight - templateHeight + 1U)
                * templateWidth
                * templateHeight;
        };

        // The two real authored elements this default was found wanting by.
        auto constexpr narrowAnchor = searchCost(90, 33, 180, 70);
        auto constexpr widestAnchor = searchCost(200, 50, 480, 90);
        static_assert(narrowAnchor == 10'270'260U);
        static_assert(widestAnchor == 115'210'000U);

        // evaluatePage shares one budget across every page anchor in the catalog,
        // so the figure to cover is the sum over a whole evaluation: eight pages
        // of two anchors each, all as costly as the widest.
        auto constexpr anchorsPerPageEvaluation = uint64{16};
        auto constexpr pageEvaluation = widestAnchor * anchorsPerPageEvaluation;
        CHECK(k_defaultPixelComparisonBudget >= pageEvaluation);
        CHECK(k_defaultPixelComparisonBudget >= narrowAnchor);

        // The ceiling stays real: a small template over a whole 1600x900 frame
        // has to be asked for with --budget, or nothing fails closed.
        auto constexpr fullFrameSmallTemplate = searchCost(66, 46, 1600, 900);
        static_assert(fullFrameSmallTemplate == 3'984'522'300U);
        CHECK(k_defaultPixelComparisonBudget < fullFrameSmallTemplate);
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
            {"--task", "missing required argument --task"},
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
        raw.emplace_back("--recognition-timeout");
        raw.emplace_back("soon");

        auto const result = parse(raw);
        REQUIRE_FALSE(result.has_value());
        CHECK(
            result.error().message()
            == "--recognition-timeout expects an integer, got \"soon\""
        );
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

    TEST_CASE("parseRunArguments refuses a zero run ceiling")
    {
        // A zero ceiling puts the run's deadline at its own start instant, so
        // the task would die at the first safepoint. It is refused by name
        // rather than read as "absent" and silently replaced by the default.
        auto raw = minimalArgs();
        raw.emplace_back("--max-runtime");
        raw.emplace_back("0");

        auto const result = parse(raw);
        REQUIRE_FALSE(result.has_value());
        CHECK(
            automationErrorKind(result.error())
            == AutomationErrorKind::InvalidResource
        );
        CHECK(
            result.error().message()
            == "--max-runtime expects a positive millisecond count, got \"0\""
        );
    }

    TEST_CASE("ExitCode preserves the documented process values")
    {
        // 3 stays absent: it was ActionAbsent, and reassigning it would tell an
        // operator reading an old 3 that it meant something else.
        auto constexpr cases = std::array{
            std::pair{ExitCode::Success, uint8{0}},
            std::pair{ExitCode::Failure, uint8{1}},
            std::pair{ExitCode::TargetCompatibilityUnverified, uint8{2}},
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

    TEST_CASE("exitCodeForReport maps every run outcome to its process exit code")
    {
        // A started run reports through this function alone, so every way a run
        // can end has to land on a documented code here.
        SUBCASE("a completed run succeeds")
        {
            CHECK(
                exitCodeForReport(task::TaskRunReport{.taskName = "daily"}, false)
                == ExitCode::Success
            );
        }

        SUBCASE("a cancelled run reports the cancellation code")
        {
            CHECK(
                exitCodeForReport(
                    reportFailedWith(AutomationErrorKind::Cancelled),
                    false
                )
                == ExitCode::Cancelled
            );
        }

        SUBCASE("a failed run reports its own kind's code")
        {
            auto constexpr cases = std::array{
                std::pair{AutomationErrorKind::Timeout, ExitCode::Timeout},
                std::pair{
                    AutomationErrorKind::TargetCompatibilityUnverified,
                    ExitCode::TargetCompatibilityUnverified,
                },
                std::pair{AutomationErrorKind::CaptureStalled, ExitCode::Failure},
                std::pair{AutomationErrorKind::IoFailure, ExitCode::Failure},
                std::pair{
                    AutomationErrorKind::InternalInvariant,
                    ExitCode::Failure,
                },
            };

            for (auto const& [kind, code] : cases)
            {
                CHECK(exitCodeForReport(reportFailedWith(kind), false) == code);
            }
        }

        SUBCASE("a requested stop takes precedence over the failure's own code")
        {
            CHECK(
                exitCodeForReport(
                    reportFailedWith(AutomationErrorKind::CaptureStalled),
                    true
                )
                == ExitCode::Cancelled
            );
        }

        SUBCASE("a run that completed as a stop arrived still succeeds")
        {
            CHECK(
                exitCodeForReport(task::TaskRunReport{.taskName = "daily"}, true)
                == ExitCode::Success
            );
        }
    }
}
