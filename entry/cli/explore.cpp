#include "explore.hpp"

#include "args.hpp"
#include "explore-cursor.hpp"
#include "explore-protocol.hpp"

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <task/exploration-session.hpp>
#include <task/task-host.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace uf::cli
{
    namespace
    {
        // How often the session looks for newly appended chunks. It bounds
        // latency and nothing else -- no guarantee depends on it -- and it is the
        // session's own plumbing rather than agent policy.
        inline constexpr auto k_queuePollInterval = std::chrono::milliseconds{25};

        [[nodiscard]]
        auto invalid(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        [[nodiscard]]
        auto pathFailure(
            std::string_view operation,
            std::filesystem::path const& path,
            std::error_code error
        ) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::IoFailure,
                std::format(
                    "cannot {} path {}: {}",
                    operation,
                    path.string(),
                    error.message()
                )
            );
        }

        [[nodiscard]]
        auto canonicalize(
            std::filesystem::path const& path,
            std::string_view role
        ) -> Result<std::filesystem::path>
        {
            if (path.empty())
            {
                return invalid(std::format("{} path must not be empty", role));
            }

            auto error          = std::error_code{};
            auto const absolute = std::filesystem::absolute(path, error);
            if (error)
            {
                return pathFailure("resolve", path, error);
            }
            auto canonical = std::filesystem::weakly_canonical(absolute, error);
            if (error)
            {
                return pathFailure("canonicalize", path, error);
            }
            return canonical;
        }

        // One chunk read out of the queue, with the byte offset just past its
        // terminator. The offset travels with the text because that is what the
        // cursor advances to, and deriving it a second time in the loop is where
        // an off-by-one would replay a chunk.
        struct FramedLine final
        {
            std::string line{};
            uintmax     endOffset{};
        };

        // Reads whole lines appended to the queue since the last call, starting
        // from the cursor's position.
        //
        // The offset is durable, not merely per-session: a partial trailing line
        // is held back until its terminator arrives, and a line is handed out
        // exactly once however many times the queue is polled and however many
        // times the session is restarted.
        class QueueReader final
        {
            std::filesystem::path m_path;

            uintmax     m_offset;
            std::string m_pending{};

        public:
            QueueReader(std::filesystem::path path, uintmax offset) noexcept
                : m_path{std::move(path)}
                , m_offset{offset}
            {
            }

            [[nodiscard]] auto readAvailable() -> Result<std::vector<FramedLine>>
            {
                auto error      = std::error_code{};
                auto const size = std::filesystem::file_size(m_path, error);
                if (error)
                {
                    return pathFailure("read", m_path, error);
                }
                if (size < m_offset)
                {
                    // A queue that shrank was replaced or truncated under the
                    // session, which makes the offset name a different file's
                    // bytes -- and the cursor beside it a record of chunks that
                    // are no longer there.
                    return invalid(
                        std::format(
                            "the chunk queue {} shrank; an exploration session "
                            "reads one append-only queue",
                            m_path.string()
                        )
                    );
                }
                if (size == m_offset)
                {
                    return std::vector<FramedLine>{};
                }

                auto stream = std::ifstream{m_path, std::ios::binary};
                if (!stream.is_open())
                {
                    return invalid(
                        std::format("cannot open the chunk queue {}", m_path.string())
                    );
                }
                stream.seekg(static_cast<std::streamoff>(m_offset));

                auto appended = std::string{};
                appended.resize(static_cast<std::size_t>(size - m_offset));
                stream.read(
                    appended.data(),
                    static_cast<std::streamsize>(appended.size())
                );
                if (stream.bad())
                {
                    return invalid(
                        std::format("cannot read the chunk queue {}", m_path.string())
                    );
                }
                appended.resize(static_cast<std::size_t>(stream.gcount()));

                auto const base = m_offset - m_pending.size();
                m_pending += appended;
                m_offset += appended.size();

                auto lines = std::vector<FramedLine>{};
                auto start = std::size_t{0};
                while (true)
                {
                    auto const newline = m_pending.find('\n', start);
                    if (newline == std::string::npos)
                    {
                        break;
                    }
                    auto line = m_pending.substr(start, newline - start);
                    if (!line.empty() && line.back() == '\r')
                    {
                        line.pop_back();
                    }
                    auto const endOffset = base + newline + 1U;
                    if (!line.empty())
                    {
                        lines.emplace_back(
                            FramedLine{
                                .line      = std::move(line),
                                .endOffset = endOffset,
                            }
                        );
                    }
                    start = newline + 1U;
                }
                m_pending.erase(0, start);
                return lines;
            }
        };

        // Appends one result line per chunk and flushes after each, so an agent
        // reading the file sees a chunk's answer before the next one runs.
        class ResultWriter final
        {
            std::ofstream m_stream;

        public:
            explicit ResultWriter(std::ofstream stream) noexcept
                : m_stream{std::move(stream)}
            {
            }

            [[nodiscard]]
            static auto create(std::filesystem::path const& path) -> Result<ResultWriter>
            {
                auto stream = std::ofstream{};
                stream.open(path, std::ios::binary | std::ios::app);
                if (!stream.is_open())
                {
                    return invalid(
                        std::format("cannot open the results file {}", path.string())
                    );
                }
                return ResultWriter{std::move(stream)};
            }

            [[nodiscard]] auto write(std::string_view line) -> Status
            {
                m_stream << line << '\n';
                m_stream.flush();
                if (!m_stream)
                {
                    return fail(
                        AutomationErrorKind::IoFailure,
                        "cannot append to the explore results file"
                    );
                }
                return ok();
            }
        };
    }

    auto validateExploreIpcPaths(ExploreArgs const& args) -> Result<ExploreIpcPaths>
    {
        UF_TRY_VALUE(queue, canonicalize(args.queue, "explore queue"));
        UF_TRY_VALUE(results, canonicalize(args.results, "explore results"));

        auto error             = std::error_code{};
        auto const queueStatus = std::filesystem::status(queue, error);
        if (error && error != std::errc::no_such_file_or_directory)
        {
            return pathFailure("inspect", queue, error);
        }
        if (!std::filesystem::is_regular_file(queueStatus))
        {
            return invalid(
                std::format(
                    "the explore queue {} must be an existing file the agent "
                    "appends to",
                    args.queue.string()
                )
            );
        }

        if (queue == results)
        {
            return invalid("the explore queue and results paths must be distinct");
        }

        auto const cursorPath = exploreQueueCursorPath(queue);
        if (cursorPath == results)
        {
            return invalid(
                std::format(
                    "the results path {} is where this queue's cursor lives",
                    args.results.string()
                )
            );
        }

        UF_TRY_VALUE(recorded, readExploreQueueCursor(cursorPath, queue));
        UF_TRY_VALUE(extent, measureExploreQueue(queue));
        UF_TRY_VALUE(start, resolveExploreQueueStart(recorded, extent, queue));

        error                    = std::error_code{};
        auto const resultsStatus = std::filesystem::symlink_status(results, error);
        if (error && error != std::errc::no_such_file_or_directory)
        {
            return pathFailure("inspect", results, error);
        }
        auto const resultsExist = (
            !error
            && resultsStatus.type() != std::filesystem::file_type::not_found
        );

        if (recorded.has_value())
        {
            if (!resultsExist)
            {
                return invalid(
                    std::format(
                        "the cursor beside {} records {} answered chunk(s), but "
                        "the results file {} is gone; a resumed session appends "
                        "to the answers it already gave",
                        args.queue.string(),
                        recorded->consumedChunks,
                        args.results.string()
                    )
                );
            }
        }
        else if (resultsExist)
        {
            return invalid(
                std::format(
                    "the results path {} already exists and no cursor records a "
                    "session it belongs to; a fresh exploration session's results "
                    "must be a fresh file",
                    args.results.string()
                )
            );
        }

        return ExploreIpcPaths{
            .queue   = std::move(queue),
            .results = std::move(results),
            .cursor  = cursorPath,
            .start   = start,
        };
    }

    auto executeExploreChunk(
        task::ExplorationSession& session,
        ExploreChunk const& chunk
    ) -> ExploreExecution
    {
        auto value = session.evaluate(chunk.chunk, chunk.id);

        // Read AFTER the chunk, which is after the session reclaimed it: the
        // figure the agent needs is the live set it is carrying into the next
        // chunk, not the garbage this one happened to leave behind.
        auto const heap = session.heapUsage();
        if (!value)
        {
            auto error     = std::move(value).error();
            auto execution = ExploreExecution{
                .resultLine = exploreFailure(chunk.id, error, heap),
            };

            // A chunk that raised is an ordinary outcome and the session goes on.
            // A CANCELLATION is not: once the generation is spent every later
            // primitive refuses on the terminal latch, so continuing would spin
            // the queue until the idle timeout and fill the results file with the
            // same refusal. The session ends on it, and run.finished reports it.
            //
            // The latch is asked rather than only the kind, because a chunk can
            // catch what was raised and return normally while the generation stays
            // spent -- and then the FIRST failing line would be a later one whose
            // error says nothing about what actually ended the run.
            if (session.terminalKind().has_value())
            {
                execution.stopSession = true;
                execution.failure     = std::move(error);
            }
            return execution;
        }

        auto execution = ExploreExecution{
            .resultLine = exploreSuccess(chunk.id, *value, heap),
        };
        if (auto const terminal = session.terminalKind(); terminal.has_value())
        {
            // The chunk swallowed a terminal raise and returned a value. The
            // value is still what it returned and the line still says so, but the
            // session is over.
            execution.stopSession = true;
            execution.failure     = fail(
                *terminal,
                "the exploration generation was spent while a chunk was running"
            ).error();
        }
        return execution;
    }

    auto exploreSession(
        task::ExplorationSession& session,
        ExploreArgs const& args,
        ExploreIpcPaths const& paths,
        std::stop_token cancellation
    ) -> Result<task::TaskRunReport>
    {
        auto reader = QueueReader{paths.queue, paths.start.consumedBytes};
        UF_TRY_VALUE(writer, ResultWriter::create(paths.results));
        UF_TRY_VALUE(
            cursor,
            ExploreQueueCursor::open(paths.cursor, paths.queue, paths.start)
        );

        auto failure      = std::optional<Error>{};
        auto stopped      = false;
        auto lastActivity = MonotonicInstant::now();
        while (!stopped)
        {
            if (cancellation.stop_requested())
            {
                failure = fail(
                    AutomationErrorKind::Cancelled,
                    "the exploration session was cancelled"
                ).error();
                break;
            }

            auto lines = reader.readAvailable();
            if (!lines)
            {
                failure = std::move(lines).error();
                break;
            }

            for (auto const& framed : *lines)
            {
                auto parsed = parseExploreChunk(framed.line);

                // The result line goes out FIRST and the cursor advances after
                // it, so a hard kill between the two costs a replay of this one
                // chunk rather than a chunk the agent was never told about.
                auto const line = parsed
                    ? std::string{}
                    : serializeExploreParseFailure(parsed.error());

                auto execution = ExploreExecution{};
                if (parsed)
                {
                    execution = executeExploreChunk(session, *parsed);
                }
                else
                {
                    // A refused line is an ordinary outcome the agent reads and
                    // corrects, never something that ends the session.
                    execution.resultLine = line;
                }

                auto written = writer.write(execution.resultLine);
                if (!written)
                {
                    // A results file that cannot be written IS session-ending,
                    // and it ends the session THROUGH `failure` rather than by
                    // returning: a return here would leave the run bracket open,
                    // and a trace whose run.finished is missing is worse evidence
                    // than one that closes on the failure.
                    failure = std::move(written).error();
                    stopped = true;
                    break;
                }

                auto advanced = cursor.advance(framed.endOffset);
                if (!advanced)
                {
                    failure = std::move(advanced).error();
                    stopped = true;
                    break;
                }

                lastActivity = MonotonicInstant::now();
                if (execution.failure.has_value())
                {
                    failure = std::move(execution.failure);
                }
                if (execution.stopSession)
                {
                    stopped = true;
                    break;
                }
            }
            if (stopped)
            {
                break;
            }

            auto const now = MonotonicInstant::now();
            if (now.saturatingDurationSince(lastActivity) >= args.idleTimeout)
            {
                break;
            }
            std::this_thread::sleep_for(k_queuePollInterval);
        }

        return session.finish(std::move(failure));
    }
}
