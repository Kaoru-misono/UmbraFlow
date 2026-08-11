#include <cli/args.hpp>
#include <cli/cli-result.hpp>

#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <task/task-host.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <source_location>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::cli
{
    namespace
    {
        template <typename Rep, typename Period>
        [[nodiscard]]
        auto asDuration(
            std::chrono::duration<Rep, Period> value
        ) -> MonotonicInstant::Duration
        {
            return std::chrono::duration_cast<MonotonicInstant::Duration>(value);
        }

        [[nodiscard]]
        auto minimalExploreArgs() -> std::vector<std::string>
        {
            return {
                "--project",
                "proj",
                "--hwnd",
                "0x20",
                "--queue",
                "queue.jsonl",
                "--results",
                "results.jsonl",
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
                .failure = errorOfKind(kind),
            };
        }
    }

    TEST_CASE("CLI error rendering includes its originating source location")
    {
        auto const location = std::source_location::current();
        auto failure = fail(
            AutomationErrorKind::InvalidResource,
            "bad project",
            {},
            location
        );

        auto const rendered = formatError(failure.error());
        auto origin = std::filesystem::path{location.file_name()}
                          .filename()
                          .string();
        origin += ':';
        origin += std::to_string(location.line());
        CHECK(rendered.find(origin) != std::string::npos);
    }

    // formatError is the only rendering of an Error in the tree, so every field
    // one carries has to survive into it or the field reaches nobody. The
    // classification it names is the automation kind rather than the detail
    // code's category: that is the vocabulary an operator of this binary and its
    // exit codes already read.
    TEST_CASE("CLI error rendering carries every field the Error was given")
    {
        auto const native = std::error_code{5, std::system_category()};
        auto failure = fail(
            AutomationErrorKind::CaptureUnavailable,
            "cannot open project",
            native
        );
        failure.error().addContext("binding the target");

        auto const rendered = formatError(failure.error());
        CHECK(rendered.find("CaptureUnavailable") != std::string::npos);
        CHECK(rendered.find("cannot open project") != std::string::npos);
        CHECK(rendered.find("binding the target") != std::string::npos);

        // The originating cause, category and value together: a bare number
        // could be the source line the same text ends with.
        auto cause = std::string{native.category().name()};
        cause += ' ';
        cause += std::to_string(native.value());
        CHECK(rendered.find(cause) != std::string::npos);
    }

    TEST_CASE("parseExploreArguments accepts the complete privileged entry shape")
    {
        auto const result = parseExploreArguments(
            std::vector<std::string>{
                "--project",
                "project-root",
                "--hwnd",
                "0x7f",
                "--queue",
                "queue.jsonl",
                "--results",
                "results.jsonl",
                "--budget",
                "4096",
                "--recognition-timeout",
                "125",
                "--max-frame-age",
                "250",
                "--idle-timeout",
                "7",
                "--trace",
                "trace.jsonl",
                "--ocr-models",
                "models",
            }
        );

        REQUIRE(result.has_value());
        CHECK(result->project == std::filesystem::path{"project-root"});
        CHECK(result->windowHandle == intptr{0x7f});
        CHECK(result->queue == std::filesystem::path{"queue.jsonl"});
        CHECK(result->results == std::filesystem::path{"results.jsonl"});
        CHECK(result->budget == uint64{4096});
        CHECK(
            result->recognitionTimeout
            == asDuration(std::chrono::milliseconds{125})
        );
        CHECK(
            result->maxFrameAge
            == asDuration(std::chrono::milliseconds{250})
        );
        CHECK(result->idleTimeout == asDuration(std::chrono::seconds{7}));
        CHECK(result->trace == std::filesystem::path{"trace.jsonl"});
        CHECK(result->ocrModels == std::filesystem::path{"models"});
    }

    TEST_CASE("parseExploreArguments applies safe defaults")
    {
        auto const result = parseExploreArguments(minimalExploreArgs());
        REQUIRE(result.has_value());
        CHECK(result->budget == k_defaultPixelComparisonBudget);
        CHECK(result->recognitionTimeout == k_defaultRecognitionTimeout);
        CHECK(result->maxFrameAge == k_defaultMaxFrameAge);
        CHECK(result->idleTimeout == k_defaultExploreIdleTimeout);
        CHECK(result->trace == std::filesystem::path{k_defaultTracePath});
        CHECK_FALSE(result->ocrModels.has_value());
    }

    TEST_CASE("parseExploreArguments requires every authority-bearing locator")
    {
        auto const required = std::array{
            std::string{"--project"},
            std::string{"--hwnd"},
            std::string{"--queue"},
            std::string{"--results"},
        };
        for (auto const& missing : required)
        {
            auto raw = minimalExploreArgs();
            auto const found = std::ranges::find(raw, missing);
            REQUIRE(found != raw.end());
            raw.erase(found, found + 2);

            INFO("missing flag: ", missing);
            CHECK_FALSE(parseExploreArguments(raw).has_value());
        }
    }

    TEST_CASE("parseExploreArguments rejects alternate and malformed input shapes")
    {
        auto const invalidCases = std::array{
            std::vector<std::string>{"--unknown", "value"},
            std::vector<std::string>{"--project"},
            std::vector<std::string>{
                "--project", "p", "--hwnd", "32", "--queue", "q",
                "--results", "r",
            },
            std::vector<std::string>{
                "--project", "p", "--hwnd", "0x0", "--queue", "q",
                "--results", "r",
            },
            std::vector<std::string>{
                "--project", "p", "--hwnd", "0x20", "--queue", "q",
                "--results", "r", "--budget", "many",
            },
            std::vector<std::string>{
                "--project", "p", "--hwnd", "0x20", "--queue", "q",
                "--results", "r", "--idle-timeout", "soon",
            },
        };

        for (auto const& raw : invalidCases)
        {
            CHECK_FALSE(parseExploreArguments(raw).has_value());
        }
    }

    // The verb that turns a project directory into registered plugins takes one
    // required path and refuses everything else. Each refusal is asserted on its
    // message rather than only on the failure: parseExploreArguments refuses the
    // same four shapes with the same kind, so a case asking whether the parse
    // failed would be satisfied by the wrong parser entirely.
    TEST_CASE("parseOpenArguments takes one required project directory")
    {
        auto const accepted = parseOpenArguments(
            std::vector<std::string>{"--project", "project-root"}
        );
        REQUIRE(accepted.has_value());
        CHECK(accepted->project == std::filesystem::path{"project-root"});

        auto const missing = parseOpenArguments(std::vector<std::string>{});
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().message().contains("--project"));

        auto const noValue = parseOpenArguments(
            std::vector<std::string>{"--project"}
        );
        REQUIRE_FALSE(noValue.has_value());
        CHECK(noValue.error().message().contains("missing value"));

        // A flag `explore` takes is refused rather than accepted and ignored.
        // This verb reaches no target, so a caller that spelled one is running
        // the wrong command and has to be told which word was wrong.
        auto const foreign = parseOpenArguments(
            std::vector<std::string>{"--project", "p", "--hwnd", "0x20"}
        );
        REQUIRE_FALSE(foreign.has_value());
        CHECK(foreign.error().message().contains("--hwnd"));
    }

    TEST_CASE("public usage names every command this binary dispatches")
    {
        auto const usage = usageText();
        CHECK(usage.find("  umbra-flow explore ") != std::string::npos);
        CHECK(usage.find("  umbra-flow open ") != std::string::npos);
        CHECK(usage.find("  umbra-flow targets\n") != std::string::npos);
        CHECK(usage.find("  umbra-flow run ") == std::string::npos);
        CHECK(usage.find("  umbra-flow check ") == std::string::npos);
        CHECK(usage.find("  umbra-flow replay ") == std::string::npos);
    }

    TEST_CASE("ExitCode uses the compact current command contract")
    {
        auto const values = std::array{
            std::pair{ExitCode::Success, uint8{0}},
            std::pair{ExitCode::Failure, uint8{1}},
            std::pair{ExitCode::TargetCompatibilityUnverified, uint8{2}},
            std::pair{ExitCode::Timeout, uint8{3}},
            std::pair{ExitCode::Cancelled, uint8{4}},
        };
        for (auto const& [code, value] : values)
        {
            CHECK(std::to_underlying(code) == value);
        }
    }

    TEST_CASE("exit-code mapping preserves automation failure meaning")
    {
        auto const cases = std::array{
            std::pair{AutomationErrorKind::Cancelled, ExitCode::Cancelled},
            std::pair{AutomationErrorKind::Timeout, ExitCode::Timeout},
            std::pair{
                AutomationErrorKind::TargetCompatibilityUnverified,
                ExitCode::TargetCompatibilityUnverified
            },
            std::pair{AutomationErrorKind::CaptureStalled, ExitCode::Failure},
            std::pair{AutomationErrorKind::IoFailure, ExitCode::Failure},
        };

        for (auto const& [kind, code] : cases)
        {
            CHECK(exitCodeForError(errorOfKind(kind), false) == code);
            CHECK(exitCodeForTaskReport(reportFailedWith(kind), false) == code);
        }

        CHECK(
            exitCodeForError(
                errorOfKind(AutomationErrorKind::CaptureStalled),
                true
            )
            == ExitCode::Cancelled
        );
        CHECK(
            exitCodeForTaskReport(task::TaskRunReport{}, false)
            == ExitCode::Success
        );
    }
}
