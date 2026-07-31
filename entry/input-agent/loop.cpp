#include "loop.hpp"

#include "agent.hpp"
#include "cursor.hpp"
#include "error-text.hpp"
#include "json-string.hpp"
#include "platform/windows-file-writer.hpp"
#include "protocol.hpp"

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>

#include <trace/event.hpp>

#include <cstddef>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

namespace uf::input_agent
{
    namespace
    {
        // The two answers the loop owns, because neither needs the session: a
        // line that never parsed, and the operator asking the run to end.
        [[nodiscard]]
        auto decideCommandOutcome(
            std::string_view line,
            IInputAgentSession& session
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
            return session.execute(*command);
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
        platform::FileWriter writer,
        std::string stamp
    ) noexcept
        : m_writer{std::move(writer)}
        , m_stamp{std::move(stamp)}
    {
    }

    auto InputAgentResultWriter::create(
        std::filesystem::path const& path,
        trace::FrontEnd frontEnd
    ) -> Result<InputAgentResultWriter>
    {
        UF_TRY_VALUE(
            writer,
            platform::FileWriter::openAppend(path)
        );
        return InputAgentResultWriter{
            std::move(writer),
            std::format(
                "{{{}:{},",
                escapeJsonString(k_inputAgentFrontEndMember),
                escapeJsonString(trace::frontEndWireName(frontEnd))
            ),
        };
    }

    auto InputAgentResultWriter::write(std::string_view line) -> Status
    {
        // Every answer this agent produces is a JSON object with at least one
        // member, so the stamp can open the object and the answer follows from
        // its first key. A line that is not one would make the whole file
        // unreadable rather than one entry wrong, which is why this is checked
        // in release too.
        UF_CHECK(line.starts_with("{\""));

        auto record = m_stamp;
        record += line.substr(1U);
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
        IInputAgentSession& session,
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
                auto const outcome = decideCommandOutcome(entry.text, session);
                session.clearCommandAudit();
                UF_TRY(results.write(outcome.resultLine));
                // Advancing after the results line is the deliberate order: a
                // hard kill in the gap replays this one command, where the
                // reverse order would silently skip an action whose delivery
                // nobody can still observe.
                UF_TRY(cursor.advance(entry.consumedBytes));
                if (outcome.stopAgent)
                {
                    UF_TRY(session.close());
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
                UF_TRY(session.close());
                UF_TRY(results.flush());
                return ok();
            }
            clock.waitForNextPoll();
        }
    }
}
