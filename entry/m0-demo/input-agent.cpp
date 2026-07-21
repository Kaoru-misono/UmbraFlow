#include "input-agent.hpp"

#include "args.hpp"
#include "capture-output.hpp"
#include "input-agent-protocol.hpp"
#include "json-string.hpp"
#include "log-jsonl.hpp"
#include "path-validation.hpp"
#include "pipeline.hpp"
#include "platform/windows-file-writer.hpp"
#include "target-setup.hpp"

#include <controller/capture.hpp>
#include <controller/detail/input-revalidation.hpp>
#include <controller/discovery.hpp>
#include <controller/dpi.hpp>
#include <controller/input.hpp>
#include <controller/target.hpp>
#include <core/error/contracts.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>
#include <domain/detection.hpp>
#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace uf::m0_demo
{
    namespace
    {
        constexpr auto g_inputAgentPollInterval = std::chrono::milliseconds{100};
        constexpr auto g_queueReadBytesPerPoll = std::size_t{64} * 1024U;
        constexpr auto g_maximumPendingQueueBytes = std::size_t{1024} * 1024U;

        [[nodiscard]]
        auto currentIoError() -> std::error_code
        {
            if (errno != 0)
            {
                return std::error_code{errno, std::generic_category()};
            }
            return std::make_error_code(std::io_errc::stream);
        }

        [[nodiscard]]
        auto agentIoFailure(
            std::string_view operation,
            std::filesystem::path const& path,
            std::error_code error
        ) -> std::unexpected<Error>
        {
            return fail(
                ErrorCode::Io,
                automationErrorDetailCode(
                    AutomationErrorKind::InvalidResource
                ),
                std::format(
                    "input-agent failed to {} {}: {}",
                    operation,
                    path.string(),
                    error.message()
                ),
                static_cast<int64>(error.value())
            );
        }
    }

    InputAgentQueueReader::InputAgentQueueReader(
        std::filesystem::path path
    ) noexcept
        : m_path{std::move(path)}
    {
    }

    auto InputAgentQueueReader::extractLines()
        -> Result<std::vector<std::string>>
    {
        auto lines = std::vector<std::string>{};
        auto consumed = std::size_t{};
        while (true)
        {
            auto const newline = m_pending.find('\n', consumed);
            if (newline == std::string::npos)
            {
                break;
            }

            auto commandBytes = newline - consumed;
            if (
                commandBytes != 0U
                && m_pending[newline - 1U] == '\r'
            )
            {
                --commandBytes;
            }
            if (commandBytes > g_maximumPendingQueueBytes)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "input-agent queue {} has a command exceeding {} bytes",
                        m_path.string(),
                        g_maximumPendingQueueBytes
                    )
                );
            }

            auto line = m_pending.substr(consumed, newline - consumed);
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            lines.emplace_back(std::move(line));
            consumed = newline + 1U;
        }
        if (consumed != 0U)
        {
            m_pending.erase(0, consumed);
        }
        if (m_pending.size() > g_maximumPendingQueueBytes)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "input-agent queue {} has an unterminated command exceeding {} bytes",
                    m_path.string(),
                    g_maximumPendingQueueBytes
                )
            );
        }
        return lines;
    }

    auto InputAgentQueueReader::create(
        std::filesystem::path path
    ) -> Result<InputAgentQueueReader>
    {
        errno = 0;
        auto stream = std::ifstream{path, std::ios::binary};
        if (!stream.is_open())
        {
            return agentIoFailure("open queue file", path, currentIoError());
        }
        return InputAgentQueueReader{std::move(path)};
    }

    auto InputAgentQueueReader::readAvailable()
        -> Result<std::vector<std::string>>
    {
        auto fileError = std::error_code{};
        auto const size = std::filesystem::file_size(m_path, fileError);
        if (fileError)
        {
            return agentIoFailure("inspect queue file", m_path, fileError);
        }
        if (size < m_offset)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "input-agent queue {} was truncated; it must be append-only",
                    m_path.string()
                )
            );
        }
        if (size == m_offset)
        {
            return extractLines();
        }

        auto const available = size - m_offset;
        auto const readBytes = static_cast<uintmax>(
            std::min<uintmax>(
                available,
                g_queueReadBytesPerPoll
            )
        );
        auto const streamOffset = checkedCast<std::streamoff>(m_offset);
        auto const streamSize = checkedCast<std::streamsize>(readBytes);
        auto const stringSize = checkedCast<std::size_t>(readBytes);
        if (!streamOffset || !streamSize || !stringSize)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "input-agent queue {} is too large to address",
                    m_path.string()
                )
            );
        }

        errno = 0;
        auto stream = std::ifstream{m_path, std::ios::binary};
        if (!stream.is_open())
        {
            return agentIoFailure("open queue file", m_path, currentIoError());
        }
        stream.seekg(*streamOffset);
        if (!stream)
        {
            return agentIoFailure("seek queue file", m_path, currentIoError());
        }

        auto chunk = std::string(*stringSize, '\0');
        errno = 0;
        stream.read(chunk.data(), *streamSize);
        if (stream.gcount() != *streamSize)
        {
            return agentIoFailure("read queue file", m_path, currentIoError());
        }
        m_offset += readBytes;
        m_pending += chunk;
        return extractLines();
    }

    namespace
    {
        class ResultWriter final
        {
            platform::FileWriter m_writer;

            explicit ResultWriter(
                platform::FileWriter writer
            ) noexcept
                : m_writer{std::move(writer)}
            {
            }

        public:
            ResultWriter(ResultWriter const&) = delete;
            auto operator=(ResultWriter const&) -> ResultWriter& = delete;
            ResultWriter(ResultWriter&&) noexcept = default;
            auto operator=(ResultWriter&&) noexcept -> ResultWriter& = default;
            ~ResultWriter() = default;

            [[nodiscard]]
            static auto create(
                std::filesystem::path const& path
            ) -> Result<ResultWriter>
            {
                UF_TRY_VALUE(
                    writer,
                    platform::FileWriter::openAppend(path)
                );
                return ResultWriter{std::move(writer)};
            }

            [[nodiscard]] auto write(std::string_view line) -> Status
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

            [[nodiscard]] auto flush() -> Status
            {
                return m_writer.flushDurably();
            }
        };

        struct OutputFile final
        {
            std::filesystem::path m_path;
            platform::FileWriter m_writer;
        };

        [[nodiscard]]
        auto openOutputFile(
            std::filesystem::path path,
            std::filesystem::path const& canonicalOutputDirectory
        ) -> Result<OutputFile>
        {
            UF_TRY_VALUE(
                writer,
                platform::FileWriter::createExclusive(
                    path,
                    canonicalOutputDirectory
                )
            );
            return OutputFile{
                .m_path = std::move(path),
                .m_writer = std::move(writer),
            };
        }

        [[nodiscard]]
        auto captureToOutput(
            WgcCaptureSession& session,
            OutputFile& output
        ) -> Result<Frame>
        {
            UF_TRY_VALUE(frame, session.capture());
            UF_TRY_VALUE(
                encoded,
                encodeFramePng(frame, output.m_path)
            );
            UF_TRY(
                output.m_writer.write(
                    std::span<std::byte const>{encoded}
                )
            );
            UF_TRY(output.m_writer.flushDurably());
            return frame;
        }

        template <typename Value>
        auto appendOptionalNumber(
            std::string& output,
            std::optional<Value> value
        ) -> void
        {
            output += value ? std::to_string(*value) : "null";
        }

        [[nodiscard]]
        auto serializeInvalidCommand(Error const& error) -> std::string
        {
            return std::format(
                "{{\"op\":null,\"ok\":false,\"error\":{}}}",
                escapeJsonString(
                    formatAutomationError(error)
                )
            );
        }

        [[nodiscard]]
        auto serializeCaptureError(Error const& error) -> std::string
        {
            return std::format(
                "{{\"op\":\"capture\",\"ok\":false,\"frame_id\":null,"
                "\"frame_size\":null,\"client_size\":null,\"delta\":null,"
                "\"error\":{}}}",
                escapeJsonString(
                    formatAutomationError(error)
                )
            );
        }

        [[nodiscard]]
        auto serializeCaptureSuccess(
            Frame const& frame,
            ClientSize client
        ) -> std::string
        {
            auto const deltaWidth = (
                static_cast<int64>(frame.width())
                - static_cast<int64>(client.width())
            );
            auto const deltaHeight = (
                static_cast<int64>(frame.height())
                - static_cast<int64>(client.height())
            );
            return std::format(
                "{{\"op\":\"capture\",\"ok\":true,\"frame_id\":{},"
                "\"frame_size\":{{\"width\":{},\"height\":{}}},"
                "\"client_size\":{{\"width\":{},\"height\":{}}},"
                "\"delta\":{{\"width\":{},\"height\":{}}},\"error\":null}}",
                frame.id().value(),
                frame.width(),
                frame.height(),
                client.width(),
                client.height(),
                deltaWidth,
                deltaHeight
            );
        }

        struct ClickResult final
        {
            std::optional<uint64> m_beforeFrame;
            std::optional<uint64> m_afterFrame;
            bool m_delivered{};
            std::optional<Error> m_error;
        };

        [[nodiscard]] auto serializeClickResult(ClickResult const& result) -> std::string
        {
            auto output = std::string{"{\"op\":\"click\",\"ok\":"};
            output += result.m_error ? "false" : "true";
            output += ",\"before_frame_id\":";
            appendOptionalNumber(output, result.m_beforeFrame);
            output += ",\"after_frame_id\":";
            appendOptionalNumber(output, result.m_afterFrame);
            output += ",\"delivered\":";
            output += result.m_delivered ? "true" : "false";
            output += ",\"error\":";
            if (result.m_error)
            {
                output += escapeJsonString(
                    formatAutomationError(*result.m_error)
                );
            }
            else
            {
                output += "null";
            }
            output += '}';
            return output;
        }

        struct CommandExecution final
        {
            std::string m_resultLine;
            bool m_stopAgent{};
        };

        [[nodiscard]]
        auto finishClick(
            ClickResult const& result,
            bool stopAgent = false
        ) -> CommandExecution
        {
            return CommandExecution{
                .m_resultLine = serializeClickResult(result),
                .m_stopAgent = stopAgent,
            };
        }

        [[nodiscard]] auto serializeQuit() -> std::string
        {
            return "{\"op\":\"quit\",\"ok\":true,\"error\":null}";
        }

        [[nodiscard]]
        auto validateOutputPath(
            std::filesystem::path const& output,
            std::filesystem::path const& canonicalOutputDirectory,
            std::filesystem::path const& canonicalQueue,
            std::filesystem::path const& canonicalResults,
            std::string_view role
        ) -> Result<std::filesystem::path>
        {
            UF_TRY_VALUE(
                canonicalOutput,
                resolveConfinedOutputPath(
                    canonicalOutputDirectory,
                    output,
                    role
                )
            );
            UF_TRY_VALUE(
                aliasesQueue,
                canonicalPathsAlias(
                    canonicalOutput,
                    canonicalQueue
                )
            );
            UF_TRY_VALUE(
                aliasesResults,
                canonicalPathsAlias(
                    canonicalOutput,
                    canonicalResults
                )
            );
            if (aliasesQueue || aliasesResults)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "{} path {} aliases an input-agent IPC file",
                        role,
                        output.string()
                    )
                );
            }
            return canonicalOutput;
        }

        [[nodiscard]]
        auto executeCapture(
            InputAgentCaptureCommand const& command,
            WgcCaptureSession& session,
            ClientSize client,
            std::filesystem::path const& canonicalOutputDirectory,
            std::filesystem::path const& canonicalQueue,
            std::filesystem::path const& canonicalResults
        ) -> std::string
        {
            auto const validated = validateOutputPath(
                command.m_output,
                canonicalOutputDirectory,
                canonicalQueue,
                canonicalResults,
                "capture output"
            );
            if (!validated)
            {
                return serializeCaptureError(validated.error());
            }

            auto output = openOutputFile(
                *validated,
                canonicalOutputDirectory
            );
            if (!output)
            {
                return serializeCaptureError(output.error());
            }

            auto captured = captureToOutput(session, *output);
            if (!captured)
            {
                return serializeCaptureError(captured.error());
            }
            return serializeCaptureSuccess(*captured, client);
        }

        auto addReleaseFailures(
            Error& error,
            std::vector<ReleaseOutcome> const& releases
        ) -> void
        {
            for (auto const& release : releases)
            {
                if (!release.m_result)
                {
                    error.addContext(
                        "input-agent click compensation failed: "
                        + formatAutomationError(
                            release.m_result.error()
                        )
                    );
                }
            }
        }

        [[nodiscard]]
        auto executeClick(
            InputAgentClickCommand const& command,
            ResolvedTarget& resolved,
            WgcCaptureSession& session,
            DeliveryTarget const& delivery,
            HeldInputs& held,
            AuditLog& audit,
            std::filesystem::path const& canonicalOutputDirectory,
            std::filesystem::path const& canonicalQueue,
            std::filesystem::path const& canonicalResults
        ) -> CommandExecution
        {
            auto result = ClickResult{};
            auto canonicalBefore = validateOutputPath(
                command.m_outputBefore,
                canonicalOutputDirectory,
                canonicalQueue,
                canonicalResults,
                "click out_before"
            );
            if (!canonicalBefore)
            {
                result.m_error = std::move(canonicalBefore).error();
                return finishClick(result);
            }
            auto canonicalAfter = validateOutputPath(
                command.m_outputAfter,
                canonicalOutputDirectory,
                canonicalQueue,
                canonicalResults,
                "click out_after"
            );
            if (!canonicalAfter)
            {
                result.m_error = std::move(canonicalAfter).error();
                return finishClick(result);
            }
            auto pathsAlias = canonicalPathsAlias(
                *canonicalBefore,
                *canonicalAfter
            );
            if (!pathsAlias)
            {
                result.m_error = std::move(pathsAlias).error();
                return finishClick(result);
            }
            if (*pathsAlias)
            {
                auto failure = fail(
                    AutomationErrorKind::InvalidResource,
                    "input-agent click out_before and out_after paths alias"
                );
                result.m_error = std::move(failure).error();
                return finishClick(result);
            }

            if (command.m_settle > g_maximumInputAgentSettle)
            {
                auto failure = fail(
                    AutomationErrorKind::InvalidResource,
                    "input-agent click settle_ms exceeds the 5000 ms limit"
                );
                result.m_error = std::move(failure).error();
                return finishClick(result);
            }

            auto beforeOutput = openOutputFile(
                *canonicalBefore,
                canonicalOutputDirectory
            );
            if (!beforeOutput)
            {
                result.m_error = std::move(beforeOutput).error();
                return finishClick(result);
            }
            auto afterOutput = openOutputFile(
                *canonicalAfter,
                canonicalOutputDirectory
            );
            if (!afterOutput)
            {
                result.m_error = std::move(afterOutput).error();
                return finishClick(result);
            }

            auto before = captureToOutput(session, *beforeOutput);
            if (!before)
            {
                result.m_error = std::move(before).error();
                return finishClick(result);
            }
            result.m_beforeFrame = before->id().value();

            auto lease = ObservationLease::forFrame(
                *before,
                g_defaultMaxActionFrameAge
            );
            if (!lease)
            {
                result.m_error = std::move(lease).error();
                return finishClick(result);
            }
            auto const point = Point<ClientSpace>{
                command.m_x,
                command.m_y
            };
            auto coordinate = validateInputAgentClick(
                delivery,
                *lease,
                point,
                MonotonicInstant::now()
            );
            if (!coordinate)
            {
                result.m_error = std::move(coordinate).error();
                return finishClick(result);
            }

            auto revalidated = resolved.revalidate();
            if (!revalidated)
            {
                result.m_error = std::move(revalidated).error();
                return finishClick(result, true);
            }
            auto unchanged = requireUnchangedTarget(*revalidated);
            if (!unchanged)
            {
                result.m_error = std::move(unchanged).error();
                return finishClick(result, true);
            }
            auto instance = session.validateTargetInstance();
            if (!instance)
            {
                result.m_error = std::move(instance).error();
                return finishClick(result, true);
            }

            auto clicked = click(
                delivery,
                *lease,
                point,
                held,
                audit
            );
            if (!clicked)
            {
                auto error = std::move(clicked).error();
                auto cleanupTarget = delivery;
                auto instanceAfterFailure = session.validateTargetInstance();
                auto const stopAgent = !instanceAfterFailure;
                if (!instanceAfterFailure)
                {
                    error.addContext(
                        "input-agent click compensation blocked because the capture target instance changed: "
                        + formatAutomationError(
                            instanceAfterFailure.error()
                        )
                    );
                    auto rejectedTarget = DeliveryTarget::create(
                        delivery.windowHandle(),
                        SessionId{~delivery.sessionId().value()},
                        delivery.generation(),
                        delivery.clientWidth(),
                        delivery.clientHeight()
                    );
                    UF_CHECK(rejectedTarget.has_value());
                    cleanupTarget = *rejectedTarget;
                }
                auto const releases = releaseHeld(cleanupTarget, held, audit);
                addReleaseFailures(error, releases);
                result.m_error = std::move(error);
                return finishClick(result, stopAgent);
            }
            result.m_delivered = true;

            if (command.m_settle > MonotonicInstant::Duration::zero())
            {
                std::this_thread::sleep_for(command.m_settle);
            }
            auto after = captureToOutput(session, *afterOutput);
            if (!after)
            {
                result.m_error = std::move(after).error();
                return finishClick(result);
            }
            result.m_afterFrame = after->id().value();
            return finishClick(result);
        }
    }

    auto clearInputAgentCommandAudit(AuditLog& audit) noexcept -> void
    {
        audit = AuditLog{};
    }

    auto validateInputAgentClick(
        DeliveryTarget const& target,
        ObservationLease lease,
        Point<ClientSpace> point,
        MonotonicInstant now
    ) -> Status
    {
        auto validated = controller_detail::checkPointerPreconditions(
            lease,
            target.sessionId(),
            target.generation(),
            now,
            point,
            target.clientWidth(),
            target.clientHeight()
        );
        if (!validated)
        {
            return std::unexpected{std::move(validated).error()};
        }
        return ok();
    }

    auto runInputAgent(
        std::span<std::string const> raw
    ) -> Status
    {
        UF_TRY_VALUE(args, parseInputAgentArguments(raw));
        UF_TRY_VALUE(
            canonicalQueue,
            canonicalizePathForComparison(args.m_queue, "input-agent queue")
        );
        UF_TRY_VALUE(
            canonicalResults,
            canonicalizePathForComparison(
                args.m_results,
                "input-agent results"
            )
        );
        UF_TRY_VALUE(
            canonicalOutputDirectory,
            canonicalizeOutputDirectory(args.m_outputDirectory)
        );
        UF_TRY_VALUE(
            ipcPathsAlias,
            canonicalPathsAlias(canonicalQueue, canonicalResults)
        );
        if (ipcPathsAlias)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "input-agent queue and results paths must be distinct"
            );
        }
        if (
            isPathWithinDirectory(
                canonicalQueue,
                canonicalOutputDirectory
            )
            || isPathWithinDirectory(
                canonicalResults,
                canonicalOutputDirectory
            )
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "input-agent queue and results files must be outside the output directory"
            );
        }

        UF_TRY_VALUE(
            reader,
            InputAgentQueueReader::create(canonicalQueue)
        );
        UF_TRY_VALUE(writer, ResultWriter::create(canonicalResults));
        UF_TRY(ensurePerMonitorAwareV2());

        UF_TRY_VALUE(candidates, enumerateCandidates());
        auto const selectorArgs = SelectorArgs{
            .m_windowHandle = args.m_windowHandle,
        };
        auto const selector = buildSelector(selectorArgs);
        UF_TRY_VALUE(resolved, resolveTarget(candidates, selector));
        auto const client = resolved.clientSize();
        UF_TRY(ensureClientAreaUsable(client));

        auto const sessionId = SessionId{1};
        UF_TRY_VALUE(
            delivery,
            DeliveryTarget::create(
                resolved.windowHandle(),
                sessionId,
                resolved.currentGeneration(),
                client.width(),
                client.height()
            )
        );
        UF_TRY_VALUE(
            session,
            createCaptureSession(
                resolved,
                sessionId,
                WgcCaptureOptions{}
            )
        );

        auto held = HeldInputs{};
        auto audit = AuditLog{};
        auto lastActivity = MonotonicInstant::now();
        while (true)
        {
            UF_TRY_VALUE(lines, reader.readAvailable());
            for (auto const& line : lines)
            {
                auto command = parseInputAgentCommand(line);
                if (!command)
                {
                    clearInputAgentCommandAudit(audit);
                    UF_TRY(writer.write(serializeInvalidCommand(command.error())));
                    lastActivity = MonotonicInstant::now();
                    continue;
                }

                if (std::holds_alternative<InputAgentQuitCommand>(*command))
                {
                    clearInputAgentCommandAudit(audit);
                    UF_TRY(writer.write(serializeQuit()));
                    UF_TRY(session.close());
                    UF_TRY(writer.flush());
                    return ok();
                }

                auto resultLine = std::string{};
                auto stopAgent = false;
                if (auto const* capture = std::get_if<InputAgentCaptureCommand>(&*command))
                {
                    resultLine = executeCapture(
                        *capture,
                        session,
                        client,
                        canonicalOutputDirectory,
                        canonicalQueue,
                        canonicalResults
                    );
                }
                else if (auto const* clickCommand = std::get_if<InputAgentClickCommand>(
                    &*command
                ))
                {
                    auto execution = executeClick(
                        *clickCommand,
                        resolved,
                        session,
                        delivery,
                        held,
                        audit,
                        canonicalOutputDirectory,
                        canonicalQueue,
                        canonicalResults
                    );
                    resultLine = std::move(execution.m_resultLine);
                    stopAgent = execution.m_stopAgent;
                }
                else
                {
                    UF_UNREACHABLE_MSG(
                        "input-agent parsed an unsupported command variant"
                    );
                }
                clearInputAgentCommandAudit(audit);
                UF_TRY(writer.write(resultLine));
                if (stopAgent)
                {
                    UF_TRY(session.close());
                    UF_TRY(writer.flush());
                    return ok();
                }
                lastActivity = MonotonicInstant::now();
            }

            auto const now = MonotonicInstant::now();
            if (
                now.saturatingDurationSince(lastActivity)
                >= args.m_idleTimeout
            )
            {
                UF_TRY(session.close());
                UF_TRY(writer.flush());
                return ok();
            }
            std::this_thread::sleep_for(g_inputAgentPollInterval);
        }
    }
}
