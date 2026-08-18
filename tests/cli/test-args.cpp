#include <cli/args.hpp>
#include <cli/cli-result.hpp>

#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <ocr/engine.hpp>

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

    // The verb an agent reads a screenshot with. Its two required paths are
    // asserted on their messages: a verb whose model directory went missing
    // would otherwise read every image as empty and report success.
    TEST_CASE("parseOcrArguments accepts the complete measurement shape")
    {
        auto const result = parseOcrArguments(
            std::vector<std::string>{
                "--image",
                "capture.png",
                "--ocr-models",
                "models",
                "--rect",
                "440,600,300,140",
                "--layout",
                "single-line",
                "--max-lines",
                "8",
            }
        );

        REQUIRE(result.has_value());
        CHECK(result->image == std::filesystem::path{"capture.png"});
        CHECK(result->ocrModels == std::filesystem::path{"models"});
        REQUIRE(result->rect.has_value());
        // NOLINTBEGIN(bugprone-unchecked-optional-access): REQUIRE above proved engagement.
        CHECK(result->rect->x() == uint32{440});
        CHECK(result->rect->y() == uint32{600});
        CHECK(result->rect->width() == uint32{300});
        CHECK(result->rect->height() == uint32{140});
        // NOLINTEND(bugprone-unchecked-optional-access)
        CHECK(result->layout == ocr::TextLayout::SingleLine);
        CHECK(result->maximumLines == uint32{8});
    }

    // Absent --rect reads the whole image and absent --max-lines imposes no
    // ceiling, both of which the engine spells as an empty optional. Block is
    // the default because a caller that knew where the line was would have
    // passed a rectangle.
    TEST_CASE("parseOcrArguments defaults to reading all of the image")
    {
        auto const result = parseOcrArguments(
            std::vector<std::string>{"--image", "a.png", "--ocr-models", "m"}
        );

        REQUIRE(result.has_value());
        CHECK_FALSE(result->rect.has_value());
        CHECK_FALSE(result->maximumLines.has_value());
        CHECK(result->layout == ocr::TextLayout::Block);
    }

    TEST_CASE("parseOcrArguments requires both the image and the models")
    {
        auto const noImage = parseOcrArguments(
            std::vector<std::string>{"--ocr-models", "m"}
        );
        REQUIRE_FALSE(noImage.has_value());
        CHECK(noImage.error().message().contains("--image"));

        auto const noModels = parseOcrArguments(
            std::vector<std::string>{"--image", "a.png"}
        );
        REQUIRE_FALSE(noModels.has_value());
        CHECK(noModels.error().message().contains("--ocr-models"));
    }

    // A rectangle is four numbers or it is not a rectangle. Three components
    // and five are both refused rather than completed or truncated: this value
    // decides which pixels are read, and a caller that mistyped it must not be
    // handed text measured somewhere else.
    TEST_CASE("parseOcrArguments refuses a rect that is not four numbers")
    {
        auto const malformed = std::array{
            std::string{"440,600,300"},
            std::string{"440,600,300,140,7"},
            std::string{"440,600,300,"},
            std::string{"440;600;300;140"},
            std::string{"440,600,300,-140"},
            std::string{"440,600,0,140"},
            std::string{""},
        };

        for (auto const& value : malformed)
        {
            INFO("rect value: ", value);
            CHECK_FALSE(
                parseOcrArguments(
                    std::vector<std::string>{
                        "--image", "a.png", "--ocr-models", "m",
                        "--rect", value,
                    }
                )
                    .has_value()
            );
        }
    }

    TEST_CASE("parseOcrArguments accepts only the two layouts the engine has")
    {
        auto const block = parseOcrArguments(
            std::vector<std::string>{
                "--image", "a.png", "--ocr-models", "m", "--layout", "block",
            }
        );
        REQUIRE(block.has_value());
        CHECK(block->layout == ocr::TextLayout::Block);

        auto const unknown = parseOcrArguments(
            std::vector<std::string>{
                "--image", "a.png", "--ocr-models", "m", "--layout", "paragraph",
            }
        );
        REQUIRE_FALSE(unknown.has_value());
        CHECK(unknown.error().message().contains("--layout"));

        // A flag `explore` takes, refused rather than accepted and ignored:
        // this verb binds no window, so a caller that named one is running the
        // wrong command.
        auto const foreign = parseOcrArguments(
            std::vector<std::string>{
                "--image", "a.png", "--ocr-models", "m", "--hwnd", "0x20",
            }
        );
        REQUIRE_FALSE(foreign.has_value());
        CHECK(foreign.error().message().contains("--hwnd"));
    }

    TEST_CASE("parseUpgradeArguments accepts the complete upgrade shape")
    {
        auto const result = parseUpgradeArguments(
            std::vector<std::string>{
                "--project",
                "project-root",
                "--runtime",
                "runtime-root",
                "--handoff",
                "handoff-root",
                "--release-manifest-hash",
                "sha256:" + std::string(64, 'a'),
                "--artifact-root-hash",
                "sha256:" + std::string(64, 'b'),
                "--capability",
                "operate",
                "--capability",
                "reobserve",
            }
        );
        REQUIRE(result.has_value());
        CHECK(result->project == std::filesystem::path{"project-root"});
        CHECK(result->runtime == std::filesystem::path{"runtime-root"});
        CHECK(result->handoff == std::filesystem::path{"handoff-root"});
        CHECK(result->releaseManifestHash.hex() == std::string(64, 'a'));
        CHECK(result->artifactRootHash.hex() == std::string(64, 'b'));
        REQUIRE(result->capabilities.size() == 2U);
        CHECK(result->capabilities[0] == "operate");
        CHECK(result->capabilities[1] == "reobserve");
    }

    TEST_CASE("parseUpgradeArguments requires every authority-bearing locator")
    {
        auto const noHandoff = parseUpgradeArguments(
            std::vector<std::string>{
                "--project", "p", "--runtime", "r",
                "--release-manifest-hash", "sha256:" + std::string(64, 'a'),
                "--artifact-root-hash", "sha256:" + std::string(64, 'b'),
            }
        );
        REQUIRE_FALSE(noHandoff.has_value());
        CHECK(noHandoff.error().message().contains("--handoff"));

        // A hash the ledger could never match is refused at the boundary,
        // naming the flag that was wrong rather than the format alone.
        auto const badHash = parseUpgradeArguments(
            std::vector<std::string>{
                "--project", "p", "--runtime", "r", "--handoff", "h",
                "--release-manifest-hash", "not-a-hash",
                "--artifact-root-hash", "sha256:" + std::string(64, 'b'),
            }
        );
        REQUIRE_FALSE(badHash.has_value());
        CHECK(badHash.error().message().contains("--release-manifest-hash"));

        auto const missingHash = parseUpgradeArguments(
            std::vector<std::string>{
                "--project", "p", "--runtime", "r", "--handoff", "h",
                "--artifact-root-hash", "sha256:" + std::string(64, 'b'),
            }
        );
        REQUIRE_FALSE(missingHash.has_value());
        CHECK(missingHash.error().message().contains("--release-manifest-hash"));

        // A flag `explore` takes, refused rather than accepted and ignored:
        // this verb binds no window, so a caller that named one is running the
        // wrong command.
        auto const foreign = parseUpgradeArguments(
            std::vector<std::string>{
                "--project", "p", "--runtime", "r", "--handoff", "h",
                "--release-manifest-hash", "sha256:" + std::string(64, 'a'),
                "--artifact-root-hash", "sha256:" + std::string(64, 'b'),
                "--hwnd", "0x20",
            }
        );
        REQUIRE_FALSE(foreign.has_value());
        CHECK(foreign.error().message().contains("--hwnd"));
    }

    TEST_CASE("parseApproveArguments accepts the complete approval shape")
    {
        auto const result = parseApproveArguments(
            std::vector<std::string>{
                "--runtime",
                "runtime-root",
                "--artifact-root-hash",
                "sha256:" + std::string(64, 'c'),
                "--evidence-hash",
                "sha256:" + std::string(64, 'd'),
                "--capability",
                "operate",
            }
        );
        REQUIRE(result.has_value());
        CHECK(result->runtime == std::filesystem::path{"runtime-root"});
        CHECK(result->artifactRootHash.hex() == std::string(64, 'c'));
        CHECK(result->evidenceHash.hex() == std::string(64, 'd'));
        REQUIRE(result->capabilities.size() == 1U);
        CHECK(result->capabilities[0] == "operate");
    }

    TEST_CASE("parseApproveArguments requires the root and both hashes")
    {
        auto const noEvidence = parseApproveArguments(
            std::vector<std::string>{
                "--runtime", "r",
                "--artifact-root-hash", "sha256:" + std::string(64, 'c'),
            }
        );
        REQUIRE_FALSE(noEvidence.has_value());
        CHECK(noEvidence.error().message().contains("--evidence-hash"));

        auto const badHash = parseApproveArguments(
            std::vector<std::string>{
                "--runtime", "r",
                "--artifact-root-hash", "sha256:" + std::string(64, 'c'),
                "--evidence-hash", "evidence",
            }
        );
        REQUIRE_FALSE(badHash.has_value());
        CHECK(badHash.error().message().contains("--evidence-hash"));

        auto const foreign = parseApproveArguments(
            std::vector<std::string>{
                "--runtime", "r",
                "--artifact-root-hash", "sha256:" + std::string(64, 'c'),
                "--evidence-hash", "sha256:" + std::string(64, 'd'),
                "--project", "p",
            }
        );
        REQUIRE_FALSE(foreign.has_value());
        CHECK(foreign.error().message().contains("--project"));
    }

    // The name is the assertion: every command main.cpp dispatches must appear.
    // It listed four of six until 2026-08-17 -- `observe` had been missing since
    // it was added, and `reclaim` arrived the same day -- so a command could ship
    // undocumented under a case that says it cannot. Adding one to main.cpp and
    // not to usageText() reds here.
    TEST_CASE("public usage names every command this binary dispatches")
    {
        auto const usage = usageText();
        CHECK(usage.find("  umbra-flow approve ") != std::string::npos);
        CHECK(usage.find("  umbra-flow explore ") != std::string::npos);
        CHECK(usage.find("  umbra-flow observe ") != std::string::npos);
        CHECK(usage.find("  umbra-flow ocr ") != std::string::npos);
        CHECK(usage.find("  umbra-flow open ") != std::string::npos);
        CHECK(usage.find("  umbra-flow reclaim ") != std::string::npos);
        CHECK(usage.find("  umbra-flow targets\n") != std::string::npos);
        CHECK(usage.find("  umbra-flow upgrade ") != std::string::npos);
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
