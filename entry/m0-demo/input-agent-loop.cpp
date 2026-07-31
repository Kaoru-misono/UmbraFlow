#include "input-agent-loop.hpp"

#include "input-agent-cursor.hpp"
#include "input-agent-protocol.hpp"
#include "input-agent.hpp"
#include "json-string.hpp"
#include "log-jsonl.hpp"
#include "platform/windows-file-writer.hpp"

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>

#include <cstddef>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

namespace uf::m0_demo
{
    namespace
    {
        // The two answers the loop owns, because neither needs the target: a
        // line that never parsed, and the operator asking the run to end.
        [[nodiscard]]
        auto decideCommandOutcome(
            std::string_view line,
            IInputAgentTarget& target
        ) -> InputAgentCommandOutcome
        {
            auto command = parseInputAgentCommand(line);
            if (!command)
            {
                return InputAgentCommandOutcome{
                    .resultLine = serializeInvalidInputAgentCommand(
                        command.error()
                    ),
                    .stopAgent = false,
                };
            }
            if (std::holds_alternative<InputAgentQuitCommand>(*command))
            {
                return InputAgentCommandOutcome{
                    .resultLine = serializeInputAgentQuitResult(),
                    .stopAgent  = true,
                };
            }
            return target.execute(*command);
        }
    }

    auto serializeInvalidInputAgentCommand(Error const& error) -> std::string
    {
        return std::format(
            "{{\"op\":null,\"ok\":false,\"error\":{}}}",
            escapeJsonString(
                formatAutomationError(error)
            )
        );
    }

    auto serializeInputAgentQuitResult() -> std::string
    {
        return "{\"op\":\"quit\",\"ok\":true,\"error\":null}";
    }

    auto SystemInputAgentPollClock::now() const noexcept -> MonotonicInstant
    {
        return MonotonicInstant::now();
    }

    auto SystemInputAgentPollClock::waitForNextPoll() -> void
    {
        std::this_thread::sleep_for(k_inputAgentPollInterval);
    }

    InputAgentResultWriter::InputAgentResultWriter(
        platform::FileWriter writer
    ) noexcept
        : m_writer{std::move(writer)}
    {
    }

    auto InputAgentResultWriter::create(
        std::filesystem::path const& path
    ) -> Result<InputAgentResultWriter>
    {
        UF_TRY_VALUE(
            writer,
            platform::FileWriter::openAppend(path)
        );
        return InputAgentResultWriter{std::move(writer)};
    }

    auto InputAgentResultWriter::write(std::string_view line) -> Status
    {
        auto record = std::string{line};
        record += '\n';
        UF_TRY(
            m_writer.write(
                std::as_bytes(
                    std::span<char const>{
                        record.data(),
                        record.size()
                    }
                )
            )
        );
        return flush();
    }

    auto InputAgentResultWriter::flush() -> Status
    {
        return m_writer.flushDurably();
    }

    auto runInputAgentQueueLoop(
        InputAgentQueueReader& reader,
        InputAgentQueueCursor& cursor,
        InputAgentResultWriter& results,
        IInputAgentTarget& target,
        IInputAgentPollClock& clock,
        MonotonicInstant::Duration idleTimeout
    ) -> Status
    {
        auto lastActivity = clock.now();
        while (true)
        {
            UF_TRY_VALUE(entries, reader.readAvailable());
            for (auto const& entry : entries)
            {
                auto const outcome = decideCommandOutcome(entry.text, target);
                target.clearCommandAudit();
                UF_TRY(results.write(outcome.resultLine));
                // Advancing after the results line is the deliberate order: a
                // hard kill in the gap replays this one command, where the
                // reverse order would silently skip an action whose delivery
                // nobody can still observe.
                UF_TRY(cursor.advance(entry.consumedBytes));
                if (outcome.stopAgent)
                {
                    UF_TRY(target.close());
                    UF_TRY(results.flush());
                    return ok();
                }
                lastActivity = clock.now();
            }

            auto const now = clock.now();
            if (
                now.saturatingDurationSince(lastActivity)
                >= idleTimeout
            )
            {
                UF_TRY(target.close());
                UF_TRY(results.flush());
                return ok();
            }
            clock.waitForNextPoll();
        }
    }
}
