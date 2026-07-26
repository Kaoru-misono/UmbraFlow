#include <file-trace-sink.hpp>

#include <domain/error.hpp>
#include <engine/trace.hpp>

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace uf::cli
{
    namespace
    {
        [[nodiscard]]
        auto uniqueTracePath() -> std::filesystem::path
        {
            static auto counter = 0;
            ++counter;
            return std::filesystem::temp_directory_path()
                / ("uf-cli-trace-" + std::to_string(counter) + ".jsonl");
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

    TEST_CASE("FileTraceSink appends one serialized JSONL line per emit")
    {
        auto const path = uniqueTracePath();
        std::filesystem::remove(path);

        auto const first = engine::TraceEvent{
            .kind = engine::TraceEventKind::SessionStarted,
        };
        auto const second = engine::TraceEvent{
            .kind    = engine::TraceEventKind::Failure,
            .message = std::string{"boom"},
        };

        {
            auto sink = FileTraceSink::create(path);
            REQUIRE(sink.has_value());
            CHECK((*sink)->emit(first).has_value());
            CHECK((*sink)->emit(second).has_value());
        }

        auto const lines = readLines(path);
        REQUIRE(lines.size() == 2U);
        CHECK(lines[0] == engine::serializeTraceEvent(first));
        CHECK(lines[1] == engine::serializeTraceEvent(second));

        std::filesystem::remove(path);
    }

    TEST_CASE("FileTraceSink reports an unopenable trace path as an error Status")
    {
        auto const path = std::filesystem::temp_directory_path()
            / "uf-cli-trace-missing-dir"
            / "trace.jsonl";
        std::filesystem::remove_all(path.parent_path());

        auto const sink = FileTraceSink::create(path);
        REQUIRE_FALSE(sink.has_value());
        CHECK(automationErrorKind(sink.error()) == AutomationErrorKind::IoFailure);
    }
}
