#include <args.hpp>
#include <drive-protocol.hpp>
#include <drive.hpp>
#include <queue-cursor.hpp>

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

        // Unique across runs as well as within one: the guard under test refuses a
        // results path that exists, so a leftover file from an earlier run would fail
        // the fresh-output cases at their own first assertion.
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

    TEST_CASE("every primitive has a command with the primitive's own name")
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

        // The named family reaches this protocol through the same single
        // definition the other two families do.
        auto const namedKey = parseDriveCommand(
            R"({"op":"key","cycle":4,"key":"ENTER"})"
        );
        CHECK(namedKey.has_value());

        // A keystroke posts real input, so a nearly-right name is refused rather
        // than guessed at.
        for (auto const line : std::array<std::string_view, 4>{
            R"({"op":"key","cycle":4,"key":"e"})",
            R"({"op":"key","cycle":4,"key":"enter"})",
            R"({"op":"key","cycle":4,"key":"TAB"})",
            R"({"op":"key","key":"E"})",
        })
        {
            CAPTURE(line);
            CHECK_FALSE(parseDriveCommand(line).has_value());
        }
    }

    TEST_CASE("the wheel and the pointer move reach an operator too")
    {
        // Both verbs existed on the script surface with no operator spelling, so
        // a step a task took by hand could not be reproduced by hand -- which is
        // the way to tell a refused delivery from one that landed and did
        // nothing.
        auto const scrolled = parseDriveCommand(R"({"op":"scroll","cycle":2,"notches":3})");
        REQUIRE(scrolled.has_value());
        CHECK(
            std::get<DriveScrollCommand>(*scrolled)
            == DriveScrollCommand{.cycle = 2, .notches = 3}
        );

        // The two directions are one verb, so this is the only signed field in
        // the protocol and the reader has to take a sign at all.
        auto const back = parseDriveCommand(R"({"op":"scroll","cycle":2,"notches":-4})");
        REQUIRE(back.has_value());
        CHECK(std::get<DriveScrollCommand>(*back).notches == -4);

        auto const moved =
            parseDriveCommand(R"({"op":"move","cycle":7,"x":801,"y":817})");
        REQUIRE(moved.has_value());
        CHECK(
            std::get<DriveMovePointerCommand>(*moved)
            == DriveMovePointerCommand{.cycle = 7, .x = 801, .y = 817}
        );

        CHECK(driveCommandOperation(*scrolled) == "scroll");
        CHECK(driveCommandOperation(*moved) == "move");
    }

    TEST_CASE("the two new commands refuse what they cannot mean")
    {
        // A cycle orders the delivery against the observations around it, so
        // neither verb can be spelled without one.
        CHECK_FALSE(parseDriveCommand(R"({"op":"scroll","notches":1})").has_value());
        CHECK_FALSE(parseDriveCommand(R"({"op":"move","x":1,"y":2})").has_value());

        // A move with one coordinate is a move to nowhere, not a move along an
        // axis.
        CHECK_FALSE(parseDriveCommand(R"({"op":"move","cycle":1,"x":5})").has_value());
        CHECK_FALSE(parseDriveCommand(R"({"op":"move","cycle":1,"y":5})").has_value());

        // Sign is the scroll's alone: a coordinate behind the target's left edge
        // is a mistake, and PixelPoint could not carry it anyway.
        CHECK_FALSE(
            parseDriveCommand(R"({"op":"move","cycle":1,"x":-5,"y":2})").has_value()
        );
        CHECK_FALSE(parseDriveCommand(R"({"op":"scroll","cycle":1,"notches":-})")
                        .has_value());

        // Fields belonging to another command are refused rather than dropped.
        CHECK_FALSE(
            parseDriveCommand(R"({"op":"scroll","cycle":1,"notches":1,"x":2})")
                .has_value()
        );
        CHECK_FALSE(
            parseDriveCommand(R"({"op":"move","cycle":1,"x":1,"y":2,"notches":1})")
                .has_value()
        );
        CHECK_FALSE(parseDriveCommand(R"({"op":"key","cycle":1,"key":"A","notches":1})")
                        .has_value());

        // Out of range on both sides of the one signed field.
        CHECK(parseDriveCommand(R"({"op":"scroll","cycle":1,"notches":-2147483648})")
                  .has_value());
        CHECK_FALSE(
            parseDriveCommand(R"({"op":"scroll","cycle":1,"notches":-2147483649})")
                .has_value()
        );
        CHECK_FALSE(
            parseDriveCommand(R"({"op":"scroll","cycle":1,"notches":2147483648})")
                .has_value()
        );
        CHECK_FALSE(
            parseDriveCommand(R"({"op":"move","cycle":1,"x":4294967296,"y":0})")
                .has_value()
        );
    }

    TEST_CASE("the retired model verbs are refused rather than silently accepted")
    {
        // Retired with the C++ page model (docs/plans/2026-07-31-script-owned-page-model.md
        // 9). A script written against the old protocol must be TOLD, not quietly
        // dropped: each verb falls through to the unrecognized-op refusal, and the
        // fields they used to carry are unrecognized too.
        for (auto const line : std::array<std::string_view, 7>{
            R"({"op":"cycle_page","cycle":3})",
            R"({"op":"cycle_find","cycle":3,"element":"end_turn"})",
            R"({"op":"cycle_click","cycle":3,"hit":7})",
            R"({"op":"wait_page","page":"battle","timeout_ms":30000,"poll_ms":500})",
            R"({"op":"find_click","element":"end_turn","timeout_ms":5000,"poll_ms":250})",
            R"({"op":"cycle_close","cycle":3,"element":"end_turn"})",
            R"({"op":"cycle_close","cycle":3,"hit":7})",
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

        // A verb whose answer is a boolean must not read like one whose answer is
        // absent: `released` and `budget` are written out either way.
        CHECK(
            serializeDriveResult(
                "cycle_close",
                DriveResult{.ok = true, .released = false}
            )
            == R"({"op":"cycle_close","ok":true,"released":false})"
        );

        // The failure kind is the domain's own wire spelling, so an operator reads
        // the same string the trace line and a task's Tier B error carry.
        auto const failed = driveFailure(
            "key",
            fail(AutomationErrorKind::StaleObservation, "gone").error()
        );
        CHECK(
            failed
            == R"({"op":"key","ok":false,"error":"stale_observation","message":"gone"})"
        );
    }

    TEST_CASE("a drive session refuses a results path that already exists")
    {
        // A stale results file from an earlier session must never be mistaken for
        // this one's, and nothing may be silently appended to or clobbered.
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

        // The refusal left the existing file untouched.
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

    TEST_CASE("a drive session refuses a queue whose history no cursor accounts for")
    {
        // A front-end that tails its queue from byte zero re-delivers every
        // command already in it, keystrokes included, against a live target. The
        // results path is an independent argument, so a fresh one is no evidence
        // that the queue is fresh too.
        auto const queue = uniquePath("queue");
        writeFile(
            queue,
            "{\"op\":\"cycle_open\"}\n{\"op\":\"key\",\"cycle\":1,\"key\":\"E\"}\n"
        );

        auto const refused = validateDriveIpcPaths(
            driveArgsOver(queue, uniquePath("results"))
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(
            automationErrorKind(refused.error())
            == AutomationErrorKind::InvalidResource
        );
        CHECK(refused.error().message().contains("no cursor records what ran"));

        auto error = std::error_code{};
        static_cast<void>(std::filesystem::remove(queue, error));
    }

    TEST_CASE("a drive session resumes where its answers stopped")
    {
        auto const queue          = uniquePath("queue");
        auto const results        = uniquePath("results");
        constexpr auto k_answered = std::string_view{"{\"op\":\"cycle_open\"}\n"};

        writeFile(queue, "");
        auto const fresh = validateDriveIpcPaths(driveArgsOver(queue, results));
        REQUIRE(fresh.has_value());
        CHECK(fresh->start.consumedBytes == 0U);

        // What a session that answered one command and then died leaves behind:
        // its cursor, the results file it answered into, and a command the
        // operator appended afterwards.
        writeFile(queue, std::string{k_answered} + "{\"op\":\"quit\"}\n");
        writeFile(results, "{\"op\":\"cycle_open\",\"ok\":true,\"cycle\":1}\n");
        writeFile(
            fresh->cursor,
            serializeQueueCursor(
                QueueCursorRecord{
                    .queue    = fresh->queue,
                    .position = QueuePosition{
                        .consumedBytes = k_answered.size(),
                        .consumedLines = 1,
                    },
                }
            )
        );

        auto const resumed = validateDriveIpcPaths(driveArgsOver(queue, results));
        REQUIRE(resumed.has_value());
        CHECK(resumed->start.consumedBytes == k_answered.size());
        CHECK(resumed->start.consumedLines == 1U);

        auto error = std::error_code{};
        static_cast<void>(std::filesystem::remove(queue, error));
        static_cast<void>(std::filesystem::remove(results, error));
        static_cast<void>(std::filesystem::remove(fresh->cursor, error));
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

        // --task is a `run` flag and must not be spellable here: the argument
        // shapes are the first place the two modes refuse to be one session.
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
