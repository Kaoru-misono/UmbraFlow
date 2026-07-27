#include <task/file-trace-sink.hpp>
#include <task/trace.hpp>

#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace uf::task
{
    namespace
    {
        [[nodiscard]]
        auto uniqueTracePath() -> std::filesystem::path
        {
            static auto counter = 0;
            ++counter;
            return std::filesystem::temp_directory_path()
                / ("uf-task-trace-" + std::to_string(counter) + ".jsonl");
        }

        [[nodiscard]]
        auto readLines(std::filesystem::path const& path) -> std::vector<std::string>
        {
            auto stream = std::ifstream{path, std::ios::binary};
            REQUIRE(stream.is_open());
            auto lines = std::vector<std::string>{};
            auto line  = std::string{};
            while (std::getline(stream, line))
            {
                lines.emplace_back(line);
            }
            return lines;
        }
    }

    TEST_CASE("serializeTaskTraceEvent emits a minimal event as schema and kind only")
    {
        auto const event = TaskTraceEvent{.kind = TaskTraceEventKind::TaskStarted};

        auto constexpr expected = std::string_view{
            "{\"schema\":\"task-trace/v1\",\"kind\":\"TaskStarted\"}"
        };

        CHECK(serializeTaskTraceEvent(event) == expected);
    }

    TEST_CASE("serializeTaskTraceEvent emits a full TaskStarted in schema order")
    {
        auto const event = TaskTraceEvent{
            .kind        = TaskTraceEventKind::TaskStarted,
            .taskName    = std::string{"daily"},
            .scriptHash  = std::string{"abc123"},
            .luauVersion = std::string{"6"},
            .projectId   = std::string{"personal.game"},
            .seed        = uint64{42},
        };

        auto constexpr expected = std::string_view{
            "{\"schema\":\"task-trace/v1\",\"kind\":\"TaskStarted\""
            ",\"taskName\":\"daily\",\"scriptHash\":\"abc123\""
            ",\"luauVersion\":\"6\",\"projectId\":\"personal.game\",\"seed\":42}"
        };

        CHECK(serializeTaskTraceEvent(event) == expected);
    }

    TEST_CASE("serializeTaskTraceEvent sorts the resource name lists before emitting")
    {
        // The lists arrive unordered on purpose: an unordered container's
        // iteration order must never reach the wire (a determinism-ledger
        // constraint), so the serializer sorts them and the golden line is sorted
        // regardless of input order.
        auto const event = TaskTraceEvent{
            .kind        = TaskTraceEventKind::ResourcesValidated,
            .recognizers = std::vector<std::string>{"battle", "accept", "daily"},
            .pages       = std::vector<std::string>{"main", "home"},
        };

        auto constexpr expected = std::string_view{
            "{\"schema\":\"task-trace/v1\",\"kind\":\"ResourcesValidated\""
            ",\"recognizers\":[\"accept\",\"battle\",\"daily\"]"
            ",\"pages\":[\"home\",\"main\"]}"
        };

        CHECK(serializeTaskTraceEvent(event) == expected);
    }

    TEST_CASE("serializeTaskTraceEvent emits an empty resource list as an empty array")
    {
        auto const event = TaskTraceEvent{
            .kind        = TaskTraceEventKind::ResourcesValidated,
            .recognizers = std::vector<std::string>{},
            .pages       = std::vector<std::string>{},
        };

        auto constexpr expected = std::string_view{
            "{\"schema\":\"task-trace/v1\",\"kind\":\"ResourcesValidated\""
            ",\"recognizers\":[],\"pages\":[]}"
        };

        CHECK(serializeTaskTraceEvent(event) == expected);
    }

    TEST_CASE("serializeTaskTraceEvent emits a succeeding HostCall")
    {
        auto const event = TaskTraceEvent{
            .kind    = TaskTraceEventKind::HostCall,
            .verb    = std::string{"capture"},
            .outcome = HostCallOutcome::Succeeded,
        };

        auto constexpr expected = std::string_view{
            "{\"schema\":\"task-trace/v1\",\"kind\":\"HostCall\""
            ",\"verb\":\"capture\",\"outcome\":\"Succeeded\"}"
        };

        CHECK(serializeTaskTraceEvent(event) == expected);
    }

    TEST_CASE("serializeTaskTraceEvent emits an empty-result HostCall")
    {
        auto const event = TaskTraceEvent{
            .kind    = TaskTraceEventKind::HostCall,
            .verb    = std::string{"find"},
            .outcome = HostCallOutcome::Empty,
        };

        auto constexpr expected = std::string_view{
            "{\"schema\":\"task-trace/v1\",\"kind\":\"HostCall\""
            ",\"verb\":\"find\",\"outcome\":\"Empty\"}"
        };

        CHECK(serializeTaskTraceEvent(event) == expected);
    }

    TEST_CASE("serializeTaskTraceEvent emits a failing HostCall with its error kind")
    {
        // The errorKind uses the snake_case spelling a script's Tier B error table
        // reports, so a trace line names a failure exactly as the script saw it.
        auto const event = TaskTraceEvent{
            .kind      = TaskTraceEventKind::HostCall,
            .verb      = std::string{"click"},
            .outcome   = HostCallOutcome::Failed,
            .errorKind = AutomationErrorKind::StaleObservation,
        };

        auto constexpr expected = std::string_view{
            "{\"schema\":\"task-trace/v1\",\"kind\":\"HostCall\""
            ",\"verb\":\"click\",\"outcome\":\"Failed\""
            ",\"errorKind\":\"stale_observation\"}"
        };

        CHECK(serializeTaskTraceEvent(event) == expected);
    }

    TEST_CASE("serializeTaskTraceEvent emits a clean TaskFinished")
    {
        auto const event = TaskTraceEvent{
            .kind       = TaskTraceEventKind::TaskFinished,
            .exitReason = TaskExitReason::Completed,
        };

        auto constexpr expected = std::string_view{
            "{\"schema\":\"task-trace/v1\",\"kind\":\"TaskFinished\""
            ",\"exitReason\":\"Completed\"}"
        };

        CHECK(serializeTaskTraceEvent(event) == expected);
    }

    TEST_CASE("serializeTaskTraceEvent emits a failed TaskFinished with its error kind")
    {
        auto const event = TaskTraceEvent{
            .kind       = TaskTraceEventKind::TaskFinished,
            .errorKind  = AutomationErrorKind::Timeout,
            .exitReason = TaskExitReason::Failed,
        };

        auto constexpr expected = std::string_view{
            "{\"schema\":\"task-trace/v1\",\"kind\":\"TaskFinished\""
            ",\"errorKind\":\"timeout\",\"exitReason\":\"Failed\"}"
        };

        CHECK(serializeTaskTraceEvent(event) == expected);
    }

    TEST_CASE("serializeTaskTraceEvent emits a cancelled TaskFinished")
    {
        auto const event = TaskTraceEvent{
            .kind       = TaskTraceEventKind::TaskFinished,
            .exitReason = TaskExitReason::Cancelled,
        };

        auto constexpr expected = std::string_view{
            "{\"schema\":\"task-trace/v1\",\"kind\":\"TaskFinished\""
            ",\"exitReason\":\"Cancelled\"}"
        };

        CHECK(serializeTaskTraceEvent(event) == expected);
    }

    TEST_CASE("serializeTaskTraceEvent escapes quotes, backslashes, and control bytes")
    {
        auto taskName = std::string{"a\"b\\c\n"};
        taskName.push_back(static_cast<char>(0x01));

        auto const event = TaskTraceEvent{
            .kind     = TaskTraceEventKind::TaskStarted,
            .taskName = taskName,
        };

        auto constexpr expected = std::string_view{
            "{\"schema\":\"task-trace/v1\",\"kind\":\"TaskStarted\""
            ",\"taskName\":\"a\\\"b\\\\c\\n\\u0001\"}"
        };

        CHECK(serializeTaskTraceEvent(event) == expected);
    }

    TEST_CASE("FileTaskTraceSink appends one serialized JSONL line per emit")
    {
        auto const path = uniqueTracePath();
        std::filesystem::remove(path);

        auto const first = TaskTraceEvent{
            .kind        = TaskTraceEventKind::TaskStarted,
            .taskName    = std::string{"daily"},
            .scriptHash  = std::string{"abc123"},
            .luauVersion = std::string{"6"},
            .projectId   = std::string{"personal.game"},
            .seed        = uint64{42},
        };
        auto const second = TaskTraceEvent{
            .kind    = TaskTraceEventKind::HostCall,
            .verb    = std::string{"capture"},
            .outcome = HostCallOutcome::Succeeded,
        };

        {
            auto sink = FileTaskTraceSink::create(path);
            REQUIRE(sink.has_value());
            CHECK((*sink)->emit(first).has_value());
            CHECK((*sink)->emit(second).has_value());
        }

        auto const lines = readLines(path);
        REQUIRE(lines.size() == 2U);
        CHECK(lines[0] == serializeTaskTraceEvent(first));
        CHECK(lines[1] == serializeTaskTraceEvent(second));

        std::filesystem::remove(path);
    }

    TEST_CASE("FileTaskTraceSink reports an unopenable trace path as an error Status")
    {
        auto const path = std::filesystem::temp_directory_path()
            / "uf-task-trace-missing-dir"
            / "trace.jsonl";
        std::filesystem::remove_all(path.parent_path());

        auto const sink = FileTaskTraceSink::create(path);
        REQUIRE_FALSE(sink.has_value());
        CHECK(automationErrorKind(sink.error()) == AutomationErrorKind::IoFailure);
    }
}
