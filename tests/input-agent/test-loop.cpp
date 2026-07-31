#include <cursor.hpp>
#include <loop.hpp>
#include <protocol.hpp>
#include <agent.hpp>

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>
#include <core/utility/scope-exit.hpp>
#include <core/utility/variant-match.hpp>

#include <trace/event.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::input_agent
{
    namespace
    {
        constexpr auto k_keyCommand = std::string_view{
            R"({"op":"key","key":"E","out_before":"a.png","out_after":"b.png"})"
            "\n"
        };
        constexpr auto k_quitCommand = std::string_view{
            R"({"op":"quit"})"
            "\n"
        };
        // Every line the agent answers with opens with the front-end stamp; a
        // results file is an annotation session's whole evidence stream, so who
        // produced it is read before what it says.
        constexpr auto k_frontEndStamp = std::string_view{
            R"({"front_end":"annotation",)"
        };
        constexpr auto k_quitResultLine = std::string_view{
            R"({"front_end":"annotation","op":"quit","ok":true,"error":null})"
        };

        // No case here should poll anywhere near this often. It is the last
        // backstop against a case that hangs instead of reporting.
        constexpr auto k_waitCeiling = std::size_t{200};

        [[nodiscard]]
        auto milliseconds(uint32 count) -> MonotonicInstant::Duration
        {
            return std::chrono::duration_cast<MonotonicInstant::Duration>(
                std::chrono::milliseconds{count}
            );
        }

        // Far past any timeout a case sets, so one step of it always satisfies
        // an intact idle rule.
        [[nodiscard]] auto escapeTimeJump() -> MonotonicInstant::Duration
        {
            return milliseconds(3'600'000U);
        }

        [[nodiscard]]
        auto describeCommand(InputAgentCommand const& command) -> std::string
        {
            return matchVariant(
                command,
                [](InputAgentCaptureCommand const&) -> std::string
                {
                    return "capture";
                },
                [](InputAgentClickCommand const&) -> std::string
                {
                    return "click";
                },
                [](InputAgentKeyCommand const&) -> std::string
                {
                    return "key";
                },
                [](InputAgentScrollCommand const&) -> std::string
                {
                    return "scroll";
                },
                [](InputAgentReadCommand const&) -> std::string
                {
                    return "read";
                },
                [](InputAgentQuitCommand const&) -> std::string
                {
                    return "quit";
                }
            );
        }

        auto appendToFile(
            std::filesystem::path const& path,
            std::string_view content
        ) -> void
        {
            if (content.empty())
            {
                return;
            }
            auto stream = std::ofstream{
                path,
                std::ios::binary | std::ios::app
            };
            REQUIRE(stream.is_open());
            stream.write(
                content.data(),
                static_cast<std::streamsize>(content.size())
            );
            stream.flush();
            REQUIRE(stream.good());
        }

        [[nodiscard]]
        auto readLines(
            std::filesystem::path const& path
        ) -> std::vector<std::string>
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

        auto removeAllBestEffort(
            std::filesystem::path const& path
        ) noexcept -> void
        {
            try
            {
                auto error = std::error_code{};
                static_cast<void>(std::filesystem::remove_all(path, error));
            }
            catch (...)
            {
            }
        }

        // The three files one agent run works against, created empty so the
        // loop's own reader, cursor, and writer open exactly what the real
        // agent opens.
        struct AgentFiles final
        {
            std::filesystem::path directory{};
            std::filesystem::path queue{};
            std::filesystem::path results{};
        };

        [[nodiscard]]
        auto createAgentFiles(
            std::string_view role,
            std::string_view queueText
        ) -> AgentFiles
        {
            auto const token = std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();
            auto const directory = std::filesystem::temp_directory_path()
                / std::format("umbraflow-{}-{}", role, token);
            auto error = std::error_code{};
            REQUIRE(std::filesystem::create_directory(directory, error));
            REQUIRE_FALSE(error);

            auto files = AgentFiles{
                .directory = directory,
                .queue     = directory / "queue.jsonl",
                .results   = directory / "results.jsonl",
            };
            {
                auto queueStream = std::ofstream{
                    files.queue,
                    std::ios::binary | std::ios::trunc
                };
                REQUIRE(queueStream.is_open());
            }
            appendToFile(files.queue, queueText);
            return files;
        }

        // A session that answers from a script instead of from a window. Every
        // line it produces reports ok:false, because the property under test is
        // what the loop does around a command's outcome rather than what a real
        // window would have made of it.
        class ScriptedSession final : public IInputAgentSession
        {
            std::size_t m_stopAtCommand;

            std::vector<std::string> m_executed{};
            std::size_t              m_auditClears{};
            std::size_t              m_closes{};

        public:
            // stopAtCommand counts from one; zero never stops. It stands in for
            // the target having been replaced under the agent, which is the one
            // failure an action op answers and then refuses to continue past.
            explicit ScriptedSession(std::size_t stopAtCommand) noexcept
                : m_stopAtCommand{stopAtCommand}
            {
            }

            [[nodiscard]]
            auto execute(
                InputAgentCommand const& command
            ) -> InputAgentCommandOutcome override
            {
                m_executed.emplace_back(describeCommand(command));
                return InputAgentCommandOutcome{
                    .resultLine = std::format(
                        R"({{"op":"{}","ok":false,"index":{}}})",
                        m_executed.back(),
                        m_executed.size()
                    ),
                    .stopAgent = (
                        m_stopAtCommand != 0U
                        && m_executed.size() == m_stopAtCommand
                    ),
                };
            }

            auto clearCommandAudit() noexcept -> void override
            {
                ++m_auditClears;
            }

            [[nodiscard]] auto close() -> Status override
            {
                ++m_closes;
                return ok();
            }

            [[nodiscard]]
            auto executed() const -> std::vector<std::string> const&
            {
                return m_executed;
            }

            [[nodiscard]] auto auditClears() const noexcept -> std::size_t
            {
                return m_auditClears;
            }

            [[nodiscard]] auto closes() const noexcept -> std::size_t
            {
                return m_closes;
            }
        };

        // The test's scheduler as much as its clock. A queue only grows between
        // polls, so appending here is how a command arrives at a chosen poll,
        // and moving the instant on by one poll interval is exactly what the
        // real agent's sleep does to the steady clock.
        //
        // The wait budget is the escape hatch. A loop that has lost its exit
        // rule would otherwise spin here forever, and a hanging case reports
        // nothing; feeding it a quit at the budget lets the wait count fail the
        // case instead.
        class ScriptedClock final : public IInputAgentPollClock
        {
            std::filesystem::path    m_queue;
            std::vector<std::string> m_appendPerWait;
            std::size_t              m_waitBudget;

            MonotonicInstant m_now{
                MonotonicInstant::fromTimePoint(MonotonicInstant::TimePoint{})
            };

            std::size_t m_waits{};

        public:
            ScriptedClock(
                std::filesystem::path queue,
                std::vector<std::string> appendPerWait,
                std::size_t waitBudget
            )
                : m_queue{std::move(queue)}
                , m_appendPerWait{std::move(appendPerWait)}
                , m_waitBudget{waitBudget}
            {
            }

            [[nodiscard]] auto now() const noexcept -> MonotonicInstant override
            {
                return m_now;
            }

            auto waitForNextPoll() -> void override
            {
                ++m_waits;
                // A run still polling this far past its budget has lost every
                // exit the budget opens for it. Failing here is what keeps that
                // from becoming a hang: doctest unwinds out of the loop, so the
                // case reports rather than never returning.
                REQUIRE(m_waits <= k_waitCeiling);

                auto step = k_inputAgentPollInterval;
                if (m_waits < m_waitBudget)
                {
                    if (m_waits <= m_appendPerWait.size())
                    {
                        appendToFile(m_queue, m_appendPerWait[m_waits - 1U]);
                    }
                }
                else
                {
                    // At the budget both exits are opened, because a case
                    // removes one of them or the other, never both: a quit for
                    // a run that still stops when told to, and a jump past any
                    // idle timeout for a run that no longer does. The quit is
                    // appended exactly once, or a run answering one every poll
                    // would keep restarting the countdown that has to end it.
                    if (m_waits == m_waitBudget)
                    {
                        appendToFile(m_queue, k_quitCommand);
                    }
                    step = escapeTimeJump();
                }

                auto const advanced = m_now.checkedAdd(step);
                REQUIRE(advanced.has_value());
                m_now = *advanced;
            }

            [[nodiscard]] auto waits() const noexcept -> std::size_t
            {
                return m_waits;
            }
        };
    }

    TEST_CASE("input-agent loop consumes the line of a command that failed")
    {
        // The defect this guards: a command answered with ok:false looks like
        // work that did not happen, and a loop that declines to consume it
        // replays it against the session on the very next poll.
        auto const files = createAgentFiles(
            "input-agent-loop-consume",
            std::string{k_keyCommand} + std::string{k_keyCommand}
                + std::string{k_quitCommand}
        );
        auto const cleanup = scopeExit(
            [cleanupPath = files.directory]() noexcept
            {
                removeAllBestEffort(cleanupPath);
            }
        );

        auto cursor = InputAgentQueueCursor::open(
            inputAgentQueueCursorPath(files.queue),
            files.queue,
            InputAgentQueuePosition{}
        );
        REQUIRE(cursor.has_value());
        auto reader = InputAgentQueueReader::create(files.queue, uintmax{});
        REQUIRE(reader.has_value());
        auto results = InputAgentResultWriter::create(
            files.results,
            trace::FrontEnd::Annotation
        );
        REQUIRE(results.has_value());

        auto session = ScriptedSession{0U};
        auto clock   = ScriptedClock{files.queue, {}, 12U};
        auto const status = runInputAgentQueueLoop(
            *reader,
            *cursor,
            *results,
            session,
            clock,
            milliseconds(10'000U)
        );
        REQUIRE(status.has_value());

        auto const lines = readLines(files.results);
        REQUIRE(lines.size() == 3U);
        CHECK(lines[0].contains(R"("ok":false)"));
        CHECK(lines[1].contains(R"("ok":false)"));
        CHECK(lines[2] == k_quitResultLine);

        auto const queueBytes = std::filesystem::file_size(files.queue);
        CHECK(cursor->position().consumedCommands == uintmax{3});
        CHECK(cursor->position().consumedBytes == queueBytes);
        CHECK(session.executed() == std::vector<std::string>{"key", "key"});
        CHECK(session.auditClears() == 3U);
        CHECK(session.closes() == 1U);
        CHECK(clock.waits() == 0U);
    }

    TEST_CASE("input-agent loop stops where the session stopped answering")
    {
        // An action op that reports the capture target instance changed has
        // seen the window it was launched against replaced. Everything queued
        // behind it was written for the old window, so none of it may run.
        auto const files = createAgentFiles(
            "input-agent-loop-stop",
            std::string{k_keyCommand} + std::string{k_keyCommand}
                + std::string{k_keyCommand}
        );
        auto const cleanup = scopeExit(
            [cleanupPath = files.directory]() noexcept
            {
                removeAllBestEffort(cleanupPath);
            }
        );

        auto cursor = InputAgentQueueCursor::open(
            inputAgentQueueCursorPath(files.queue),
            files.queue,
            InputAgentQueuePosition{}
        );
        REQUIRE(cursor.has_value());
        auto reader = InputAgentQueueReader::create(files.queue, uintmax{});
        REQUIRE(reader.has_value());
        auto results = InputAgentResultWriter::create(
            files.results,
            trace::FrontEnd::Annotation
        );
        REQUIRE(results.has_value());

        auto session = ScriptedSession{2U};
        auto clock   = ScriptedClock{files.queue, {}, 12U};
        auto const status = runInputAgentQueueLoop(
            *reader,
            *cursor,
            *results,
            session,
            clock,
            milliseconds(10'000U)
        );
        REQUIRE(status.has_value());

        // The stopping command is still answered and still consumed: an
        // operator has to be able to read which command ended the run, and a
        // restart must not deliver it a second time.
        auto const lines = readLines(files.results);
        REQUIRE(lines.size() == 2U);
        CHECK(lines[1].contains(R"("index":2)"));
        CHECK(session.executed().size() == 2U);
        CHECK(cursor->position().consumedCommands == uintmax{2});
        CHECK(
            cursor->position().consumedBytes
            == uintmax{2} * k_keyCommand.size()
        );
        CHECK(session.closes() == 1U);
        CHECK(clock.waits() == 0U);
    }

    TEST_CASE("input-agent loop answers an invalid command and keeps serving")
    {
        auto const files = createAgentFiles(
            "input-agent-loop-invalid",
            std::string{"this is not a command\n"} + std::string{k_keyCommand}
                + std::string{k_quitCommand}
        );
        auto const cleanup = scopeExit(
            [cleanupPath = files.directory]() noexcept
            {
                removeAllBestEffort(cleanupPath);
            }
        );

        auto cursor = InputAgentQueueCursor::open(
            inputAgentQueueCursorPath(files.queue),
            files.queue,
            InputAgentQueuePosition{}
        );
        REQUIRE(cursor.has_value());
        auto reader = InputAgentQueueReader::create(files.queue, uintmax{});
        REQUIRE(reader.has_value());
        auto results = InputAgentResultWriter::create(
            files.results,
            trace::FrontEnd::Annotation
        );
        REQUIRE(results.has_value());

        auto session = ScriptedSession{0U};
        auto clock   = ScriptedClock{files.queue, {}, 12U};
        auto const status = runInputAgentQueueLoop(
            *reader,
            *cursor,
            *results,
            session,
            clock,
            milliseconds(10'000U)
        );
        REQUIRE(status.has_value());

        auto const lines = readLines(files.results);
        REQUIRE(lines.size() == 3U);
        CHECK(
            lines[0].starts_with(R"({"front_end":"annotation","op":null,"ok":false,)")
        );
        CHECK(lines[2] == k_quitResultLine);
        // Neither the unparseable text nor the quit is the session's business:
        // one never became a command, and ending the run is the loop's own
        // decision.
        CHECK(session.executed() == std::vector<std::string>{"key"});
        CHECK(cursor->position().consumedCommands == uintmax{3});
        CHECK(session.closes() == 1U);
    }

    TEST_CASE("input-agent loop stamps every answer with its front-end")
    {
        // A results file is an annotation session's whole evidence stream. The
        // session reaches no host, so it has no run and no generation and writes
        // no umbraflow-trace/v2 line; without this stamp nothing in the file says
        // an annotation session produced it, and the three lines below come from
        // three different authors -- the session, the loop refusing a line that
        // never parsed, and the loop's own quit. That is why the stamp sits where
        // all three pass rather than in any one of them.
        auto const files = createAgentFiles(
            "input-agent-loop-stamp",
            std::string{k_keyCommand} + std::string{"not a command\n"}
                + std::string{k_quitCommand}
        );
        auto const cleanup = scopeExit(
            [cleanupPath = files.directory]() noexcept
            {
                removeAllBestEffort(cleanupPath);
            }
        );

        auto cursor = InputAgentQueueCursor::open(
            inputAgentQueueCursorPath(files.queue),
            files.queue,
            InputAgentQueuePosition{}
        );
        REQUIRE(cursor.has_value());
        auto reader = InputAgentQueueReader::create(files.queue, uintmax{});
        REQUIRE(reader.has_value());
        auto results = InputAgentResultWriter::create(
            files.results,
            trace::FrontEnd::Annotation
        );
        REQUIRE(results.has_value());

        auto session = ScriptedSession{0U};
        auto clock   = ScriptedClock{files.queue, {}, 12U};
        auto const status = runInputAgentQueueLoop(
            *reader,
            *cursor,
            *results,
            session,
            clock,
            milliseconds(10'000U)
        );
        REQUIRE(status.has_value());

        auto const lines = readLines(files.results);
        REQUIRE(lines.size() == 3U);
        for (auto const& line : lines)
        {
            CAPTURE(line);
            CHECK(line.starts_with(k_frontEndStamp));
        }
        // The stamp opens the line and the answer follows unchanged, so a reader
        // meets the attribution before what it attributes.
        CHECK(lines[0].contains(R"("op":"key")"));
        CHECK(lines[1].contains(R"("op":null)"));
        CHECK(lines[2] == k_quitResultLine);

        // It is trace's spelling of trace's value, not a local one: the same
        // answer under another front-end says so.
        CHECK_FALSE(lines[0].contains(R"("front_end":"operator")"));
        CHECK(
            lines[0].starts_with(
                std::format(
                    R"({{"front_end":"{}",)",
                    trace::frontEndWireName(trace::FrontEnd::Annotation)
                )
            )
        );
    }

    TEST_CASE("input-agent loop ends a run whose queue has gone silent")
    {
        auto const files = createAgentFiles("input-agent-loop-idle", "");
        auto const cleanup = scopeExit(
            [cleanupPath = files.directory]() noexcept
            {
                removeAllBestEffort(cleanupPath);
            }
        );

        auto cursor = InputAgentQueueCursor::open(
            inputAgentQueueCursorPath(files.queue),
            files.queue,
            InputAgentQueuePosition{}
        );
        REQUIRE(cursor.has_value());
        auto reader = InputAgentQueueReader::create(files.queue, uintmax{});
        REQUIRE(reader.has_value());
        auto results = InputAgentResultWriter::create(
            files.results,
            trace::FrontEnd::Annotation
        );
        REQUIRE(results.has_value());

        auto session = ScriptedSession{0U};
        auto clock   = ScriptedClock{files.queue, {}, 12U};
        auto const status = runInputAgentQueueLoop(
            *reader,
            *cursor,
            *results,
            session,
            clock,
            milliseconds(250U)
        );
        REQUIRE(status.has_value());

        // Three polls of silence carry the clock to 300 ms, which is the first
        // reading at or past the timeout; the run ends there rather than at the
        // budget's escape hatch.
        CHECK(clock.waits() == 3U);
        CHECK(readLines(files.results).empty());
        CHECK(cursor->position() == InputAgentQueuePosition{});
        CHECK(session.executed().empty());
        CHECK(session.closes() == 1U);
    }

    TEST_CASE("input-agent loop stays alive while work keeps arriving")
    {
        // An annotation session is mostly waiting: an operator appends one
        // command, reads the frames it produced, and appends the next. Each
        // command has to restart the idle countdown, or the agent dies between
        // two commands an operator considers part of the same session.
        auto const files = createAgentFiles("input-agent-loop-busy", "");
        auto const cleanup = scopeExit(
            [cleanupPath = files.directory]() noexcept
            {
                removeAllBestEffort(cleanupPath);
            }
        );

        auto cursor = InputAgentQueueCursor::open(
            inputAgentQueueCursorPath(files.queue),
            files.queue,
            InputAgentQueuePosition{}
        );
        REQUIRE(cursor.has_value());
        auto reader = InputAgentQueueReader::create(files.queue, uintmax{});
        REQUIRE(reader.has_value());
        auto results = InputAgentResultWriter::create(
            files.results,
            trace::FrontEnd::Annotation
        );
        REQUIRE(results.has_value());

        // A command every second poll: 200 ms apart, comfortably inside a
        // 250 ms timeout that only a restarted countdown can survive.
        auto schedule = std::vector<std::string>{
            "",
            std::string{k_keyCommand},
            "",
            std::string{k_keyCommand},
            "",
            std::string{k_keyCommand},
            "",
            std::string{k_quitCommand},
        };
        auto session = ScriptedSession{0U};
        auto clock   = ScriptedClock{files.queue, std::move(schedule), 20U};
        auto const status = runInputAgentQueueLoop(
            *reader,
            *cursor,
            *results,
            session,
            clock,
            milliseconds(250U)
        );
        REQUIRE(status.has_value());

        CHECK(clock.waits() == 8U);
        CHECK(session.executed().size() == 3U);
        CHECK(readLines(files.results).size() == 4U);
        CHECK(cursor->position().consumedCommands == uintmax{4});
        CHECK(session.closes() == 1U);
    }
}
