#include "drive.hpp"

#include "args.hpp"
#include "drive-protocol.hpp"
#include "queue-cursor.hpp"
#include "queue-ipc.hpp"

#include <core/error/contracts.hpp>
#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>

#include <domain/error.hpp>

#include <task/operator-session.hpp>
#include <task/task-host.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>

namespace uf::cli
{
    namespace
    {
        // How often the session looks for newly appended command lines: it bounds
        // command latency and nothing else, and is session plumbing rather than task
        // policy, which is why it is a constant here and not a command field.
        inline constexpr auto k_queuePollInterval = std::chrono::milliseconds{25};

        // What this front-end calls its queue and itself wherever the shared
        // reader refuses a line.
        inline constexpr auto k_driveQueueNaming = QueueNaming{
            .queue   = "command queue",
            .session = "a drive session",
        };

        inline constexpr auto k_driveResultsLabel = std::string_view{"drive"};

        // A refused command is normally ordinary: the operator reads the line and
        // tries something else. A CANCELLATION is not, and that is the one
        // distinction made here -- every later primitive refuses on the terminal
        // latch, so continuing would spin the queue to the idle timeout filling the
        // results file with the same refusal.
        [[nodiscard]]
        auto refused(std::string_view operation, Error error) -> DriveExecution
        {
            auto execution = DriveExecution{
                .resultLine = driveFailure(operation, error),
            };
            if (automationErrorKind(error) == AutomationErrorKind::Cancelled)
            {
                execution.stopSession = true;
                execution.failure     = std::move(error);
            }
            return execution;
        }

        [[nodiscard]]
        auto succeeded(
            std::string_view operation,
            DriveResult result
        ) -> DriveExecution
        {
            result.ok = true;
            return DriveExecution{
                .resultLine = serializeDriveResult(operation, result),
            };
        }

    }

    auto validateDriveIpcPaths(DriveArgs const& args) -> Result<DriveIpcPaths>
    {
        UF_TRY_VALUE(queue, canonicalize(args.queue, "drive queue"));
        UF_TRY_VALUE(results, canonicalize(args.results, "drive results"));

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
                    "the drive queue {} must be an existing file the operator "
                    "appends to",
                    args.queue.string()
                )
            );
        }

        if (queue == results)
        {
            return invalid("the drive queue and results paths must be distinct");
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
                        "the cursor beside {} records {} answered command(s), but "
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
                    "session it belongs to; a fresh drive session's results "
                    "must be a fresh file",
                    args.results.string()
                )
            );
        }

        return DriveIpcPaths{
            .queue   = std::move(queue),
            .results = std::move(results),
            .cursor  = cursorPath,
            .start   = start,
        };
    }

    auto executeDriveCommand(
        task::OperatorSession& session,
        DriveCommand const& command
    ) -> DriveExecution
    {
        auto const operation = driveCommandOperation(command);

        if (std::holds_alternative<DriveQuitCommand>(command))
        {
            auto execution        = succeeded(operation, DriveResult{});
            execution.stopSession = true;
            return execution;
        }

        if (std::holds_alternative<DriveCycleOpenCommand>(command))
        {
            auto cycle = session.cycleOpen();
            if (!cycle)
            {
                return refused(operation, std::move(cycle).error());
            }
            return succeeded(operation, DriveResult{.cycle = *cycle});
        }

        if (auto const* p_close = std::get_if<DriveCycleCloseCommand>(&command))
        {
            auto released = session.cycleClose(p_close->cycle);
            if (!released)
            {
                return refused(operation, std::move(released).error());
            }
            return succeeded(operation, DriveResult{.released = *released});
        }

        if (auto const* p_key = std::get_if<DriveKeyCommand>(&command))
        {
            auto pressed = session.key(p_key->cycle, p_key->key);
            if (!pressed)
            {
                return refused(operation, std::move(pressed).error());
            }
            return succeeded(operation, DriveResult{});
        }

        if (auto const* p_settle = std::get_if<DriveSettleCommand>(&command))
        {
            auto settled = session.settle(p_settle->duration);
            if (!settled)
            {
                return refused(operation, std::move(settled).error());
            }
            return succeeded(operation, DriveResult{});
        }

        if (auto const* p_deadline = std::get_if<DriveDeadlineCommand>(&command))
        {
            auto minted = session.deadline(p_deadline->duration);
            if (!minted)
            {
                return refused(operation, std::move(minted).error());
            }
            return succeeded(operation, DriveResult{.deadline = *minted});
        }

        auto const* p_wait = std::get_if<DriveWaitCommand>(&command);
        UF_CHECK_MSG(
            p_wait != nullptr,
            "the drive protocol produced a command with no execution path"
        );
        auto budget = session.wait(p_wait->deadline, p_wait->pollInterval);
        if (!budget)
        {
            return refused(operation, std::move(budget).error());
        }
        return succeeded(operation, DriveResult{.budget = *budget});
    }

    auto driveSession(
        task::OperatorSession& session,
        DriveArgs const& args,
        DriveIpcPaths const& paths,
        std::stop_token cancellation
    ) -> Result<task::TaskRunReport>
    {
        auto reader = QueueReader{
            paths.queue,
            k_driveQueueNaming,
            paths.start.consumedBytes,
        };
        UF_TRY_VALUE(
            writer,
            ResultWriter::create(paths.results, k_driveResultsLabel)
        );
        UF_TRY_VALUE(
            cursor,
            QueueCursor::open(paths.cursor, paths.queue, paths.start)
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
                    "the operator session was cancelled"
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
                auto command = parseDriveCommand(framed.line);

                // The result line goes out FIRST and the cursor advances after it,
                // so a hard kill between the two replays this one command rather
                // than losing one the operator was never answered about.
                auto execution = DriveExecution{};
                if (command)
                {
                    execution = executeDriveCommand(session, *command);
                }
                else
                {
                    // A refused line is an ordinary outcome the operator reads and
                    // corrects, never something that ends the session.
                    execution.resultLine =
                        serializeDriveParseFailure(command.error());
                }

                auto written = writer.write(execution.resultLine);
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
