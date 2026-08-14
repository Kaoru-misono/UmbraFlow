#include "explore.hpp"

#include "args.hpp"
#include "explore-protocol.hpp"
#include "queue-cursor.hpp"
#include "queue-ipc.hpp"

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/time/poll-sleep.hpp>

#include <domain/error.hpp>

#include <task/exploration-session.hpp>
#include <task/task-host.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace uf::cli
{
    namespace
    {
        // How often the session looks for newly appended chunks: it bounds latency
        // and nothing else, and is session plumbing rather than agent policy. It is
        // below k_maxPollSleepSlice, so one poll is one uninterrupted slice.
        inline constexpr auto k_queuePollInterval = (
            std::chrono::duration_cast<MonotonicInstant::Duration>(
                std::chrono::milliseconds{25}
            )
        );

        // What this front-end calls its queue and itself wherever the shared
        // reader refuses a line.
        inline constexpr auto k_exploreQueueNaming = QueueNaming{
            .queue   = "chunk queue",
            .session = "an exploration session",
        };

        inline constexpr auto k_exploreResultsLabel = std::string_view{"explore"};

        // The instant an unanswered queue ends the session. An idle timeout the
        // clock cannot be advanced by saturates to the furthest instant it has,
        // which is the same answer the caller would get from the arithmetic
        // anyway: no elapsed span the clock can express reaches such a timeout,
        // so the session never idles out either way.
        [[nodiscard]]
        auto idleDeadlineFrom(
            MonotonicInstant from,
            MonotonicInstant::Duration idleTimeout
        ) noexcept -> MonotonicInstant
        {
            return from.checkedAdd(idleTimeout).value_or(
                MonotonicInstant::fromTimePoint(MonotonicInstant::TimePoint::max())
            );
        }
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

        auto const cursorPath = queueCursorPath(queue);
        if (cursorPath == results)
        {
            return invalid(
                std::format(
                    "the results path {} is where this queue's cursor lives",
                    args.results.string()
                )
            );
        }

        UF_TRY_VALUE(recorded, readQueueCursor(cursorPath, queue));
        UF_TRY_VALUE(extent, measureQueueExtent(queue));
        UF_TRY_VALUE(start, resolveQueueStart(recorded, extent, queue));

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
                        recorded->consumedLines,
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

        // Read AFTER the chunk, so the figure is the live set the agent carries
        // into the next chunk rather than the garbage this one left behind.
        auto const heap = session.heapUsage();
        if (!value)
        {
            auto error     = std::move(value).error();
            auto execution = ExploreExecution{
                .resultLine = exploreFailure(chunk.id, error, heap, chunk.endsSession),
            };

            // A chunk that raised is an ordinary outcome and the session goes on. A
            // CANCELLATION is not: every later primitive refuses on the terminal
            // latch, so continuing would spin the queue to the idle timeout filling
            // the results file with the same refusal. The latch is asked rather than
            // the kind, because a chunk can catch what was raised and return
            // normally while the generation stays spent.
            if (session.terminalKind().has_value())
            {
                execution.stopSession = true;
                execution.failure     = std::move(error);
            }

            // A last line is a last line whether or not it raised: the agent is
            // done sending either way, and the raise is already in its answer.
            if (chunk.endsSession)
            {
                execution.stopSession = true;
            }
            return execution;
        }

        auto execution = ExploreExecution{
            .resultLine = exploreSuccess(chunk.id, *value, heap, chunk.endsSession),
        };
        if (auto const terminal = session.terminalKind(); terminal.has_value())
        {
            // The chunk swallowed a terminal raise and returned a value. The line
            // still reports that value, but the session is over.
            execution.stopSession = true;
            execution.failure     = fail(
                *terminal,
                "the exploration generation was spent while a chunk was running"
            ).error();
        }

        // The agent said this was its last line. Answered first and stopped
        // after, so the deliberate ending is itself in the results file rather
        // than inferred from the file simply stopping.
        if (chunk.endsSession)
        {
            execution.stopSession = true;
        }
        return execution;
    }

    auto exploreSession(
        task::ExplorationSession& session,
        ExploreArgs const& args,
        ExploreIpcPaths const& paths,
        std::stop_token const& cancellation
    ) -> Result<task::TaskRunReport>
    {
        auto reader = QueueReader{
            paths.queue,
            k_exploreQueueNaming,
            paths.start.consumedBytes,
        };
        UF_TRY_VALUE(
            writer,
            ResultWriter::create(paths.results, k_exploreResultsLabel)
        );
        UF_TRY_VALUE(
            cursor,
            QueueCursor::open(paths.cursor, paths.queue, paths.start)
        );

        auto failure = std::optional<Error>{};
        auto stopped = false;

        // One deadline, so the pause and the decision to stop pausing read the
        // same instant instead of recomputing an elapsed span against each other.
        auto idleDeadline = idleDeadlineFrom(
            MonotonicInstant::now(),
            args.idleTimeout
        );
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

                // The result line goes out FIRST and the cursor advances after it,
                // so a hard kill between the two replays this one chunk rather than
                // losing a chunk the agent was never told about.
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

                auto written = writer->write(execution.resultLine);
                if (!written)
                {
                    // A results file that cannot be written is session-ending, but
                    // it ends the session through `failure` rather than by
                    // returning: a return would leave the run bracket open, and a
                    // trace missing run.finished is worse evidence.
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

                idleDeadline = idleDeadlineFrom(
                    MonotonicInstant::now(),
                    args.idleTimeout
                );
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

            if (MonotonicInstant::now() >= idleDeadline)
            {
                break;
            }

            // core's sliced sleep rather than a second copy of the slicing here:
            // it re-checks the stop token before each slice, and the interval is
            // one slice, so a stop is observed exactly as promptly as the bare
            // sleep this replaced -- and one slice sooner when the token was
            // already stopped.
            pollSleep(k_queuePollInterval, idleDeadline, cancellation);
        }

        return session.finish(std::move(failure));
    }
}
