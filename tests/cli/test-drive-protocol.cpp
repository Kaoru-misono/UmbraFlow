#include <args.hpp>
#include <drive-protocol.hpp>
#include <drive.hpp>

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/key.hpp>

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

namespace uf::cli
{
    namespace
    {
        [[nodiscard]]
        auto millis(uint64 count) -> MonotonicInstant::Duration
        {
            return std::chrono::duration_cast<MonotonicInstant::Duration>(
                std::chrono::milliseconds{count}
            );
        }

        // A temp path unique across RUNS as well as within one, which the
        // fresh-output cases require: the guard under test refuses a results path that
        // exists, so a leftover file from an earlier run -- including one a failing run
        // never got to clean up -- would refuse the case's own first assertion.
        [[nodiscard]]
        auto uniquePath(std::string_view role) -> std::filesystem::path
        {
            static auto s_sequence = std::atomic<uint64>{1};
            auto const token       = std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();
            return std::filesystem::temp_directory_path()
                / std::format(
                    "uf-drive-{}-{}-{}",
                    role,
                    token,
                    s_sequence.fetch_add(1U, std::memory_order_relaxed)
                );
        }

        auto writeFile(std::filesystem::path const& path, std::string_view text) -> void
        {
            auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
            REQUIRE(stream.is_open());
            stream << text;
            stream.flush();
            REQUIRE(stream.good());
        }

        [[nodiscard]]
        auto driveArgsOver(
            std::filesystem::path queue,
            std::filesystem::path results
        ) -> DriveArgs
        {
            return DriveArgs{
                .project  = std::filesystem::path{"project"},
                .selector = "Target",
                .queue    = std::move(queue),
                .results  = std::move(results),
            };
        }
    }

    TEST_CASE("every layer-one primitive has a command with the primitive's own name")
    {
        // "Verbatim" is the claim, so the names are pinned: an operator writes the
        // primitive's name, not a synonym, and a rename on either side breaks here.
        auto const opened = parseDriveCommand(R"({"op":"cycle_open"})");
        REQUIRE(opened.has_value());
        CHECK(std::holds_alternative<DriveCycleOpenCommand>(*opened));

        auto const closed = parseDriveCommand(R"({"op":"cycle_close","cycle":3})");
        REQUIRE(closed.has_value());
        CHECK(
            std::get<DriveCycleCloseCommand>(*closed)
            == DriveCycleCloseCommand{.cycle = 3}
        );

        auto const page = parseDriveCommand(R"({"op":"cycle_page","cycle":3})");
        REQUIRE(page.has_value());
        CHECK(
            std::get<DriveCyclePageCommand>(*page)
            == DriveCyclePageCommand{.cycle = 3}
        );

        auto const found = parseDriveCommand(
            R"({"op":"cycle_find","cycle":3,"recognizer":"end_turn"})"
        );
        REQUIRE(found.has_value());
        CHECK(
            std::get<DriveCycleFindCommand>(*found)
            == DriveCycleFindCommand{.cycle = 3, .recognizer = "end_turn"}
        );

        auto const clicked = parseDriveCommand(
            R"({"op":"cycle_click","cycle":3,"hit":7})"
        );
        REQUIRE(clicked.has_value());
        CHECK(
            std::get<DriveCycleClickCommand>(*clicked)
            == DriveCycleClickCommand{.cycle = 3, .hit = 7}
        );

        auto const settled = parseDriveCommand(R"({"op":"settle","ms":250})");
        REQUIRE(settled.has_value());
        CHECK(
            std::get<DriveSettleCommand>(*settled)
            == DriveSettleCommand{.duration = millis(250)}
        );

        auto const deadline = parseDriveCommand(R"({"op":"deadline","ms":30000})");
        REQUIRE(deadline.has_value());
        CHECK(
            std::get<DriveDeadlineCommand>(*deadline)
            == DriveDeadlineCommand{.duration = millis(30'000)}
        );

        auto const waited = parseDriveCommand(
            R"({"op":"wait","deadline":1,"poll_ms":500})"
        );
        REQUIRE(waited.has_value());
        CHECK(
            std::get<DriveWaitCommand>(*waited)
            == DriveWaitCommand{.deadline = 1, .pollInterval = millis(500)}
        );

        auto const quit = parseDriveCommand(R"({"op":"quit"})");
        REQUIRE(quit.has_value());
        CHECK(std::holds_alternative<DriveQuitCommand>(*quit));
    }

    TEST_CASE("a key command names the key its target prints, and only those")
    {
        auto const pressed = parseDriveCommand(R"({"op":"key","cycle":4,"key":"E"})");
        REQUIRE(pressed.has_value());
        auto const& command = std::get<DriveKeyCommand>(*pressed);
        CHECK(command.cycle == 4U);
        auto const expected = KeyName::create("E");
        REQUIRE(expected.has_value());
        CHECK(command.key == *expected);

        auto const functionKey = parseDriveCommand(
            R"({"op":"key","cycle":4,"key":"F12"})"
        );
        CHECK(functionKey.has_value());

        // Lower case, an unknown key and a key with no cycle are all refused: a
        // keystroke posts real input, so a name that is nearly right must not be
        // guessed at.
        for (auto const line : std::array<std::string_view, 3>{
            R"({"op":"key","cycle":4,"key":"e"})",
            R"({"op":"key","cycle":4,"key":"ENTER"})",
            R"({"op":"key","key":"E"})",
        })
        {
            CAPTURE(line);
            CHECK_FALSE(parseDriveCommand(line).has_value());
        }
    }

    TEST_CASE("a convenience command missing a policy field is rejected, not defaulted")
    {
        // THE point of the two-layer design. A convenience command carries no policy
        // defaults of its own, so every timeout and poll interval is the caller's;
        // modules/task/runtime/ctx.luau keeps the only copy of those numbers. A missing
        // field must therefore produce a refusal, never a value this binary chose.
        auto const complete = parseDriveCommand(
            R"({"op":"wait_page","page":"battle","timeout_ms":30000,"poll_ms":500})"
        );
        REQUIRE(complete.has_value());
        CHECK(
            std::get<DriveWaitPageCommand>(*complete)
            == DriveWaitPageCommand{
                .page         = "battle",
                .timeout      = millis(30'000),
                .pollInterval = millis(500),
            }
        );

        for (auto const line : std::array<std::string_view, 6>{
            // wait_page: each of the three required fields, omitted in turn.
            R"({"op":"wait_page","timeout_ms":30000,"poll_ms":500})",
            R"({"op":"wait_page","page":"battle","poll_ms":500})",
            R"({"op":"wait_page","page":"battle","timeout_ms":30000})",
            // find_click: the same, for its own three.
            R"({"op":"find_click","timeout_ms":5000,"poll_ms":250})",
            R"({"op":"find_click","recognizer":"end_turn","poll_ms":250})",
            R"({"op":"find_click","recognizer":"end_turn","timeout_ms":5000})",
        })
        {
            CAPTURE(line);
            auto const refused = parseDriveCommand(line);
            REQUIRE_FALSE(refused.has_value());
            CHECK(
                automationErrorKind(refused.error())
                == AutomationErrorKind::InvalidResource
            );
        }

        auto const findClick = parseDriveCommand(
            R"({"op":"find_click","recognizer":"end_turn","timeout_ms":5000,"poll_ms":250})"
        );
        REQUIRE(findClick.has_value());
        CHECK(
            std::get<DriveFindClickCommand>(*findClick)
            == DriveFindClickCommand{
                .recognizer   = "end_turn",
                .timeout      = millis(5'000),
                .pollInterval = millis(250),
            }
        );
    }

    TEST_CASE("the command reader refuses a line it cannot read exactly")
    {
        for (auto const line : std::array<std::string_view, 11>{
            "",
            "not json",
            R"({})",
            R"({"op":"cycle_open"} trailing)",
            R"({"op":"cycle_open","cycle":1})",
            R"({"op":"cycle_close","cycle":1,"cycle":2})",
            R"({"op":"cycle_close","cycle":01})",
            R"({"op":"settle","ms":1.5})",
            R"({"op":"settle","ms":-1})",
            R"({"op":"cycle_close","cycle":1,"nope":2})",
            R"({"op":"no_such_op"})",
        })
        {
            CAPTURE(line);
            CHECK_FALSE(parseDriveCommand(line).has_value());
        }
    }

    TEST_CASE("a result line names its command and whether it succeeded")
    {
        CHECK(
            serializeDriveResult(
                "cycle_open",
                DriveResult{.ok = true, .cycle = uint64{2}}
            )
            == R"({"op":"cycle_open","ok":true,"cycle":2})"
        );

        // A resolved page and an unresolved one must not read alike: null is a
        // completed resolution the engine already traced, not a failure.
        CHECK(
            serializeDriveResult(
                "cycle_page",
                DriveResult{
                    .ok           = true,
                    .resolvedPage = true,
                    .page         = std::string{"battle"},
                }
            )
            == R"({"op":"cycle_page","ok":true,"page":"battle"})"
        );
        CHECK(
            serializeDriveResult(
                "cycle_page",
                DriveResult{.ok = true, .resolvedPage = true}
            )
            == R"({"op":"cycle_page","ok":true,"page":null})"
        );

        // The failure kind is the domain's own wire spelling, so an operator reads the
        // same string the trace line and a task's Tier B error carry.
        auto const failed = driveFailure(
            "cycle_click",
            fail(AutomationErrorKind::StaleObservation, "gone").error()
        );
        CHECK(
            failed
            == R"({"op":"cycle_click","ok":false,"error":"stale_observation","message":"gone"})"
        );
    }

    TEST_CASE("a drive session refuses a results path that already exists")
    {
        // The fresh-output-file guarantee, carried over from the m0-demo input agent. A
        // stale results file from an earlier session must never be mistaken for this
        // one's, and nothing may be silently appended to or clobbered.
        auto const queue   = uniquePath("queue");
        auto const results = uniquePath("results");
        writeFile(queue, "");

        auto const fresh = validateDriveIpcPaths(driveArgsOver(queue, results));
        REQUIRE(fresh.has_value());

        writeFile(results, "{\"op\":\"cycle_open\",\"ok\":true}\n");
        auto const refused = validateDriveIpcPaths(driveArgsOver(queue, results));
        REQUIRE_FALSE(refused.has_value());
        CHECK(
            automationErrorKind(refused.error())
            == AutomationErrorKind::InvalidResource
        );
        CHECK(refused.error().message().contains("must be a fresh file"));

        // The refusal left the existing file untouched, which is the whole point.
        auto stream = std::ifstream{results, std::ios::binary};
        REQUIRE(stream.is_open());
        auto contents = std::string{};
        std::getline(stream, contents);
        CHECK(contents == R"({"op":"cycle_open","ok":true})");
        stream.close();

        auto error = std::error_code{};
        static_cast<void>(std::filesystem::remove(queue, error));
        static_cast<void>(std::filesystem::remove(results, error));
    }

    TEST_CASE("a drive session refuses a missing queue and an aliased results path")
    {
        auto const queue = uniquePath("queue");

        // A queue the session would have to create races the operator appending to it.
        auto const missing = validateDriveIpcPaths(
            driveArgsOver(queue, uniquePath("results"))
        );
        REQUIRE_FALSE(missing.has_value());
        CHECK(
            automationErrorKind(missing.error())
            == AutomationErrorKind::InvalidResource
        );

        writeFile(queue, "");
        auto const aliased = validateDriveIpcPaths(driveArgsOver(queue, queue));
        REQUIRE_FALSE(aliased.has_value());
        CHECK(
            automationErrorKind(aliased.error())
            == AutomationErrorKind::InvalidResource
        );

        auto error = std::error_code{};
        static_cast<void>(std::filesystem::remove(queue, error));
    }

    TEST_CASE("drive arguments require the four that name a session and share run's bounds")
    {
        auto const raw = std::array<std::string, 8>{
            "--project",
            "project",
            "--selector",
            "Target",
            "--queue",
            "queue.jsonl",
            "--results",
            "results.jsonl",
        };
        auto const parsed = parseDriveArguments(raw);
        REQUIRE(parsed.has_value());
        CHECK(parsed->budget == k_defaultPixelComparisonBudget);
        CHECK(parsed->recognitionTimeout == k_defaultRunRecognitionTimeout);
        CHECK(parsed->maxFrameAge == k_defaultRunMaxFrameAge);
        CHECK(parsed->trace == std::filesystem::path{k_defaultTracePath});

        // --task is a `run` flag and must not be spellable here: the argument shapes
        // are the first place the two modes refuse to be one session.
        auto const withTask = std::array<std::string, 10>{
            "--project",
            "project",
            "--selector",
            "Target",
            "--queue",
            "queue.jsonl",
            "--results",
            "results.jsonl",
            "--task",
            "daily",
        };
        CHECK_FALSE(parseDriveArguments(withTask).has_value());

        // ...and --queue is not a `run` flag either.
        auto const runWithQueue = std::array<std::string, 8>{
            "--project",
            "project",
            "--selector",
            "Target",
            "--task",
            "daily",
            "--queue",
            "queue.jsonl",
        };
        CHECK_FALSE(parseRunArguments(runWithQueue).has_value());

        for (auto const& omitted : std::array<std::string_view, 4>{
            "--project",
            "--selector",
            "--queue",
            "--results",
        })
        {
            CAPTURE(omitted);
            auto trimmed = std::vector<std::string>{};
            for (auto index = std::size_t{0}; index < raw.size(); index += 2U)
            {
                if (raw[index] == omitted)
                {
                    continue;
                }
                trimmed.emplace_back(raw[index]);
                trimmed.emplace_back(raw[index + 1U]);
            }
            CHECK_FALSE(parseDriveArguments(trimmed).has_value());
        }
    }
}
