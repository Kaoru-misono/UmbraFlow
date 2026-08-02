#include "drive.hpp"

#include "args.hpp"
#include "drive-protocol.hpp"

#include <core/error/contracts.hpp>
#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <task/operator-session.hpp>
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
#include <variant>
#include <vector>

namespace uf::cli
{
    namespace
    {
        // How often the session looks for newly appended command lines: it bounds
        // command latency and nothing else, and is session plumbing rather than task
        // policy, which is why it is a constant here and not a command field.
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

        // Reads whole lines appended since the last call. A line is read exactly once
        // however often the queue is polled, and a partial trailing line is held back
        // until its terminator arrives -- an operator appending a command in two
        // writes must not have half of it executed.
        class QueueReader final
        {
            std::filesystem::path m_path;

            uintmax     m_offset{};
            std::string m_pending{};

        public:
            explicit QueueReader(std::filesystem::path path) noexcept
                : m_path{std::move(path)}
            {
            }

            [[nodiscard]] auto readAvailable() -> Result<std::vector<std::string>>
            {
                auto error      = std::error_code{};
                auto const size = std::filesystem::file_size(m_path, error);
                if (error)
                {
                    return pathFailure("read", m_path, error);
                }
                if (size < m_offset)
                {
                    // A queue that shrank was replaced or truncated under the session,
                    // which makes the offset name a different file's bytes.
                    return invalid(
                        std::format(
                            "the command queue {} shrank; a drive session reads one "
                            "append-only queue",
                            m_path.string()
                        )
                    );
                }
                if (size == m_offset)
                {
                    return std::vector<std::string>{};
                }

                auto stream = std::ifstream{m_path, std::ios::binary};
                if (!stream.is_open())
                {
                    return invalid(
                        std::format(
                            "cannot open the command queue {}",
                            m_path.string()
                        )
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
                        std::format(
                            "cannot read the command queue {}",
                            m_path.string()
                        )
                    );
                }
                appended.resize(static_cast<std::size_t>(stream.gcount()));
                m_offset += appended.size();

                m_pending += appended;
                auto lines = std::vector<std::string>{};
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
                    if (!line.empty())
                    {
                        lines.emplace_back(std::move(line));
                    }
                    start = newline + 1U;
                }
                m_pending.erase(0, start);
                return lines;
            }
        };

        // Appends one result line per command and flushes after each, so an operator
        // reading the file sees a command's outcome before the next one runs.
        class ResultWriter final
        {
            std::ofstream m_stream;

        public:
            explicit ResultWriter(std::ofstream stream) noexcept
                : m_stream{std::move(stream)}
            {
            }

            [[nodiscard]]
            static auto create(
                std::filesystem::path const& path
            ) -> Result<ResultWriter>
            {
                auto stream = std::ofstream{};
                stream.open(path, std::ios::binary | std::ios::app);
                if (!stream.is_open())
                {
                    return invalid(
                        std::format(
                            "cannot open the results file {}",
                            path.string()
                        )
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
                        "cannot append to the drive results file"
                    );
                }
                return ok();
            }
        };
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

        error                    = std::error_code{};
        auto const resultsStatus = std::filesystem::symlink_status(results, error);
        if (error == std::errc::no_such_file_or_directory)
        {
            return DriveIpcPaths{
                .queue   = std::move(queue),
                .results = std::move(results),
            };
        }
        if (error)
        {
            return pathFailure("inspect", results, error);
        }
        if (resultsStatus.type() != std::filesystem::file_type::not_found)
        {
            return invalid(
                std::format(
                    "the results path {} already exists; a drive session's results "
                    "must be a fresh file",
                    args.results.string()
                )
            );
        }
        return DriveIpcPaths{
            .queue   = std::move(queue),
            .results = std::move(results),
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
        auto reader = QueueReader{paths.queue};
        UF_TRY_VALUE(writer, ResultWriter::create(paths.results));

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

            for (auto const& line : *lines)
            {
                auto command = parseDriveCommand(line);
                if (!command)
                {
                    // A refused line is an ordinary outcome the operator reads and
                    // corrects, never something that ends the session. A results file
                    // that cannot be written is session-ending, but it ends the
                    // session through `failure` rather than by returning: a return
                    // would leave the run bracket open, and a trace missing
                    // run.finished is worse evidence.
                    auto refusal = writer.write(
                        serializeDriveParseFailure(command.error())
                    );
                    if (!refusal)
                    {
                        failure = std::move(refusal).error();
                        stopped = true;
                        break;
                    }
                    lastActivity = MonotonicInstant::now();
                    continue;
                }

                auto execution = executeDriveCommand(session, *command);
                auto written   = writer.write(execution.resultLine);
                if (!written)
                {
                    failure = std::move(written).error();
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
