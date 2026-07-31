#include "test-helpers.hpp"

#include <args.hpp>
#include <cursor.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace uf::input_agent
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
    }

    TEST_CASE("input-agent arguments require file IPC paths and apply defaults")
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
        CHECK(result->windowHandle == intptr{0x1A2B});
        CHECK(result->queue == std::filesystem::path{"commands.jsonl"});
        CHECK(result->results == std::filesystem::path{"results.jsonl"});
        CHECK(result->outputDirectory == std::filesystem::path{"agent-output"});
        CHECK(result->idleTimeout == k_defaultInputAgentIdleTimeout);
        // The unstated policy is the one that asks rather than the one that
        // walks a live target back through a queue it has already run.
        CHECK(result->queueStart == InputAgentQueueStart::Refuse);
    }

    TEST_CASE("input-agent arguments name the uncursored queue policy")
    {
        auto const parseStart = [](
            std::string_view value
        ) -> Result<InputAgentArgs>
        {
            return parseInputAgentArguments(
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
                        "--queue-start",
                        value,
                    }
                )
            );
        };

        auto const beginning = parseStart("beginning");
        REQUIRE(beginning.has_value());
        CHECK(beginning->queueStart == InputAgentQueueStart::Beginning);

        auto const end = parseStart("end");
        REQUIRE(end.has_value());
        CHECK(end->queueStart == InputAgentQueueStart::End);

        auto const refuse = parseStart("refuse");
        REQUIRE(refuse.has_value());
        CHECK(refuse->queueStart == InputAgentQueueStart::Refuse);

        auto const rejected = parseStart("sometimes");
        REQUIRE_FALSE(rejected.has_value());
        test_input_agent::requireErrorKind(
            rejected.error(),
            AutomationErrorKind::InvalidResource
        );
    }

    TEST_CASE("input-agent arguments reject missing and invalid values")
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
            test_input_agent::requireErrorKind(
                result.error(),
                AutomationErrorKind::InvalidResource
            );
        }
    }
}
