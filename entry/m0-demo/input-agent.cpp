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
#include <source_location>
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
        constexpr auto k_inputAgentPollInterval = std::chrono::milliseconds{100};
        constexpr auto k_queueReadBytesPerPoll = std::size_t{64} * 1024U;
        constexpr auto k_maximumPendingQueueBytes = std::size_t{1024} * 1024U;

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
            std::error_code error,
            std::source_location location = std::source_location::current()
        ) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::IoFailure,
                std::format(
                    "input-agent failed to {} {}: {}",
                    operation,
                    path.string(),
                    error.message()
                ),
                error,
                location
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
        auto lines    = std::vector<std::string>{};
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
            if (commandBytes > k_maximumPendingQueueBytes)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "input-agent queue {} has a command exceeding {} bytes",
                        m_path.string(),
                        k_maximumPendingQueueBytes
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
        if (m_pending.size() > k_maximumPendingQueueBytes)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "input-agent queue {} has an unterminated command exceeding {} bytes",
                    m_path.string(),
                    k_maximumPendingQueueBytes
                )
            );
        }
        return lines;
    }

    auto InputAgentQueueReader::create(
        std::filesystem::path path
    ) -> Result<InputAgentQueueReader>
    {
        errno       = 0;
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
                k_queueReadBytesPerPoll
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

        errno       = 0;
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
        errno      = 0;
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
            std::filesystem::path path{};
            platform::FileWriter  writer;
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
                .path   = std::move(path),
                .writer = std::move(writer),
            };
        }

        [[nodiscard]]
        auto writeFramePng(
            Frame const& frame,
            OutputFile& output
        ) -> Status
        {
            UF_TRY_VALUE(
                encoded,
                encodeFramePng(frame, output.path)
            );
            UF_TRY(
                output.writer.write(
                    std::span<std::byte const>{encoded}
                )
            );
            UF_TRY(output.writer.flushDurably());
            return ok();
        }

        [[nodiscard]]
        auto captureToOutput(
            WgcCaptureSession& session,
            OutputFile& output
        ) -> Result<Frame>
        {
            UF_TRY_VALUE(frame, session.capture());
            UF_TRY(writeFramePng(frame, output));
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

        struct CommandExecution final
        {
            std::string resultLine{};
            bool        stopAgent{};
        };

        [[nodiscard]]
        auto finishAction(
            std::string_view operation,
            InputAgentActionResult const& result,
            bool stopAgent = false
        ) -> CommandExecution
        {
            return CommandExecution{
                .resultLine = serializeInputAgentActionResult(operation, result),
                .stopAgent  = stopAgent,
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
                command.output,
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
            std::string_view operation,
            Error& error,
            std::vector<ReleaseOutcome> const& releases
        ) -> void
        {
            for (auto const& release : releases)
            {
                if (!release.result)
                {
                    error.addContext(
                        std::format(
                            "input-agent {} compensation failed: {}",
                            operation,
                            formatAutomationError(release.result.error())
                        )
                    );
                }
            }
        }

        [[nodiscard]]
        auto checkSettleBound(
            std::string_view operation,
            MonotonicInstant::Duration settle
        ) -> Status
        {
            if (settle <= k_maximumInputAgentSettle)
            {
                return ok();
            }
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "input-agent {} settle_ms exceeds the 5000 ms limit",
                    operation
                )
            );
        }

        struct FramedAction final
        {
            OutputFile before;
            OutputFile after;

            // The observation the action is decided from. Its PNG is written
            // only after delivery; see writeFramesAfterDelivery.
            Frame frame;
        };

        // Reserves both output files before capturing, so a path that is not
        // confined, aliases another output, or already exists is refused before
        // the target is observed at all. The before-frame is deliberately left
        // unencoded here: the slow encode plus durable flush must not sit inside
        // the observe->act window, where it would inflate this observation's age
        // against max_action_frame_age and produce a false StaleObservation.
        [[nodiscard]]
        auto beginFramedAction(
            InputAgentFramePaths const& paths,
            std::filesystem::path const& canonicalOutputDirectory,
            WgcCaptureSession& session
        ) -> Result<FramedAction>
        {
            UF_TRY_VALUE(
                before,
                openOutputFile(paths.before, canonicalOutputDirectory)
            );
            UF_TRY_VALUE(
                after,
                openOutputFile(paths.after, canonicalOutputDirectory)
            );
            UF_TRY_VALUE(frame, session.capture());
            return FramedAction{
                .before = std::move(before),
                .after  = std::move(after),
                .frame  = std::move(frame),
            };
        }

        // Runs once delivery has succeeded and the observe->act window is
        // closed. The before-Frame is immutable, so its file still shows the
        // pre-action observation even though it is encoded now. The output
        // handles were reserved with CREATE_NEW before the capture, so moving
        // the encode past delivery does not weaken path confinement.
        [[nodiscard]]
        auto writeFramesAfterDelivery(
            FramedAction& action,
            MonotonicInstant::Duration settle,
            WgcCaptureSession& session
        ) -> Result<Frame>
        {
            UF_TRY(writeFramePng(action.frame, action.before));
            if (settle > MonotonicInstant::Duration::zero())
            {
                std::this_thread::sleep_for(settle);
            }
            return captureToOutput(session, action.after);
        }

        // Both action ops prove the target is still the one the observation came
        // from before posting anything. Every failure here is terminal for the
        // agent: the window it was launched against is gone or was replaced.
        // The borrows are mutable because revalidate() and
        // validateTargetInstance() refresh their own owner's continuity state;
        // nothing is returned through them.
        [[nodiscard]]
        auto ensureTargetUnchanged(
            ResolvedTarget& resolved,
            WgcCaptureSession& session
        ) -> Status
        {
            UF_TRY_VALUE(revalidated, resolved.revalidate());
            UF_TRY(requireUnchangedTarget(revalidated));
            return session.validateTargetInstance();
        }

        struct DeliveryCompensation final
        {
            Error error;
            bool  stopAgent{};
        };

        // A failed post can leave a key or the pointer button held, so the
        // release has to reach a target the controller still accepts. When the
        // capture target instance has changed, posting to it would reach a
        // window that is no longer the observed one, so the release is aimed at
        // a deliberately rejected identity: HeldInputs then drops the holds and
        // no message leaves the process.
        [[nodiscard]]
        auto compensateFailedDelivery(
            std::string_view operation,
            Error failure,
            DeliveryTarget const& delivery,
            WgcCaptureSession& session,
            HeldInputs& held,
            AuditLog& audit
        ) -> DeliveryCompensation
        {
            auto error                = std::move(failure);
            auto cleanupTarget        = delivery;
            auto instanceAfterFailure = session.validateTargetInstance();
            auto const stopAgent = !instanceAfterFailure;
            if (!instanceAfterFailure)
            {
                error.addContext(
                    std::format(
                        "input-agent {} compensation blocked because the capture target instance changed: {}",
                        operation,
                        formatAutomationError(instanceAfterFailure.error())
                    )
                );
                auto rejectedTarget = DeliveryTarget::create(
                    delivery.windowHandle(),
                    CaptureSessionId{~delivery.sessionId().value()},
                    delivery.generation(),
                    delivery.clientWidth(),
                    delivery.clientHeight()
                );
                UF_CHECK(rejectedTarget.has_value());
                cleanupTarget = *rejectedTarget;
            }
            auto const releases = releaseHeld(cleanupTarget, held, audit);
            addReleaseFailures(operation, error, releases);
            return DeliveryCompensation{
                .error     = std::move(error),
                .stopAgent = stopAgent,
            };
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
            auto constexpr operation = std::string_view{"click"};
            auto result = InputAgentActionResult{};
            auto framePaths = resolveInputAgentFramePaths(
                command.outputBefore,
                command.outputAfter,
                canonicalOutputDirectory,
                canonicalQueue,
                canonicalResults,
                operation
            );
            if (!framePaths)
            {
                result.error = std::move(framePaths).error();
                return finishAction(operation, result);
            }
            auto settleBound = checkSettleBound(operation, command.settle);
            if (!settleBound)
            {
                result.error = std::move(settleBound).error();
                return finishAction(operation, result);
            }

            auto action = beginFramedAction(
                *framePaths,
                canonicalOutputDirectory,
                session
            );
            if (!action)
            {
                result.error = std::move(action).error();
                return finishAction(operation, result);
            }
            result.beforeFrame = action->frame.id().value();

            auto lease = ObservationLease::forFrame(
                action->frame,
                k_defaultMaxActionFrameAge
            );
            if (!lease)
            {
                result.error = std::move(lease).error();
                return finishAction(operation, result);
            }
            auto const point = Point<ClientSpace>{
                command.x,
                command.y
            };
            auto coordinate = validateInputAgentClick(
                delivery,
                *lease,
                point,
                MonotonicInstant::now()
            );
            if (!coordinate)
            {
                result.error = std::move(coordinate).error();
                return finishAction(operation, result);
            }

            auto current = ensureTargetUnchanged(resolved, session);
            if (!current)
            {
                result.error = std::move(current).error();
                return finishAction(operation, result, true);
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
                auto compensation = compensateFailedDelivery(
                    operation,
                    std::move(clicked).error(),
                    delivery,
                    session,
                    held,
                    audit
                );
                result.error = std::move(compensation.error);
                return finishAction(operation, result, compensation.stopAgent);
            }
            result.delivered = true;

            auto after = writeFramesAfterDelivery(
                *action,
                command.settle,
                session
            );
            if (!after)
            {
                result.error = std::move(after).error();
                return finishAction(operation, result);
            }
            result.afterFrame = after->id().value();
            return finishAction(operation, result);
        }

        // The key path deliberately has no lease freshness fence. A keystroke
        // names no position, so an older observation cannot make it land in the
        // wrong place; the only staleness that matters is the target having been
        // replaced, which the generation check and ensureTargetUnchanged cover.
        [[nodiscard]]
        auto executeKey(
            InputAgentKeyCommand const& command,
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
            auto constexpr operation = std::string_view{"key"};
            auto result = InputAgentActionResult{};
            auto framePaths = resolveInputAgentFramePaths(
                command.outputBefore,
                command.outputAfter,
                canonicalOutputDirectory,
                canonicalQueue,
                canonicalResults,
                operation
            );
            if (!framePaths)
            {
                result.error = std::move(framePaths).error();
                return finishAction(operation, result);
            }
            auto settleBound = checkSettleBound(operation, command.settle);
            if (!settleBound)
            {
                result.error = std::move(settleBound).error();
                return finishAction(operation, result);
            }

            auto action = beginFramedAction(
                *framePaths,
                canonicalOutputDirectory,
                session
            );
            if (!action)
            {
                result.error = std::move(action).error();
                return finishAction(operation, result);
            }
            result.beforeFrame = action->frame.id().value();

            auto current = ensureTargetUnchanged(resolved, session);
            if (!current)
            {
                result.error = std::move(current).error();
                return finishAction(operation, result, true);
            }

            // keyPress posts WM_KEYDOWN then WM_KEYUP with nothing in between,
            // which is the same zero hold click uses. A hold between DOWN and UP
            // is exactly what made a hand-rolled pointer sequence read as a drag
            // and activate nothing against this target
            // (docs/pitfalls/capture-and-target-selection.md), so no wait
            // belongs inside the keystroke; settle_ms waits after it.
            auto pressed = keyPress(
                delivery,
                action->frame.targetGeneration(),
                command.key,
                held,
                audit
            );
            if (!pressed)
            {
                auto compensation = compensateFailedDelivery(
                    operation,
                    std::move(pressed).error(),
                    delivery,
                    session,
                    held,
                    audit
                );
                result.error = std::move(compensation.error);
                return finishAction(operation, result, compensation.stopAgent);
            }
            result.delivered = true;

            auto after = writeFramesAfterDelivery(
                *action,
                command.settle,
                session
            );
            if (!after)
            {
                result.error = std::move(after).error();
                return finishAction(operation, result);
            }
            result.afterFrame = after->id().value();
            return finishAction(operation, result);
        }
    }

    auto clearInputAgentCommandAudit(AuditLog& audit) noexcept -> void
    {
        audit = AuditLog{};
    }

    auto resolveInputAgentFramePaths(
        std::filesystem::path const& outputBefore,
        std::filesystem::path const& outputAfter,
        std::filesystem::path const& canonicalOutputDirectory,
        std::filesystem::path const& canonicalQueue,
        std::filesystem::path const& canonicalResults,
        std::string_view operation
    ) -> Result<InputAgentFramePaths>
    {
        auto const beforeRole = std::format("{} out_before", operation);
        auto const afterRole  = std::format("{} out_after", operation);
        UF_TRY_VALUE(
            canonicalBefore,
            validateOutputPath(
                outputBefore,
                canonicalOutputDirectory,
                canonicalQueue,
                canonicalResults,
                beforeRole
            )
        );
        UF_TRY_VALUE(
            canonicalAfter,
            validateOutputPath(
                outputAfter,
                canonicalOutputDirectory,
                canonicalQueue,
                canonicalResults,
                afterRole
            )
        );
        UF_TRY_VALUE(
            pathsAlias,
            canonicalPathsAlias(canonicalBefore, canonicalAfter)
        );
        if (pathsAlias)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "input-agent {} out_before and out_after paths alias",
                    operation
                )
            );
        }
        return InputAgentFramePaths{
            .before = std::move(canonicalBefore),
            .after  = std::move(canonicalAfter),
        };
    }

    auto serializeInputAgentActionResult(
        std::string_view operation,
        InputAgentActionResult const& result
    ) -> std::string
    {
        auto output = std::format("{{\"op\":\"{}\",\"ok\":", operation);
        output += result.error ? "false" : "true";
        output += ",\"before_frame_id\":";
        appendOptionalNumber(output, result.beforeFrame);
        output += ",\"after_frame_id\":";
        appendOptionalNumber(output, result.afterFrame);
        output += ",\"delivered\":";
        output += result.delivered ? "true" : "false";
        output += ",\"error\":";
        if (result.error)
        {
            output += escapeJsonString(
                formatAutomationError(*result.error)
            );
        }
        else
        {
            output += "null";
        }
        output += '}';
        return output;
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
            canonicalizePathForComparison(args.queue, "input-agent queue")
        );
        UF_TRY_VALUE(
            canonicalResults,
            canonicalizePathForComparison(
                args.results,
                "input-agent results"
            )
        );
        UF_TRY_VALUE(
            canonicalOutputDirectory,
            canonicalizeOutputDirectory(args.outputDirectory)
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
            .windowHandle = args.windowHandle,
        };
        auto const selector = buildSelector(selectorArgs);
        UF_TRY_VALUE(resolved, resolveTarget(candidates, selector));
        auto const client = resolved.clientSize();
        UF_TRY(ensureClientAreaUsable(client));

        auto const sessionId = CaptureSessionId{1};
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

        auto held         = HeldInputs{};
        auto audit        = AuditLog{};
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
                auto stopAgent  = false;
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
                    resultLine = std::move(execution.resultLine);
                    stopAgent  = execution.stopAgent;
                }
                else if (auto const* keyCommand = std::get_if<InputAgentKeyCommand>(
                    &*command
                ))
                {
                    auto execution = executeKey(
                        *keyCommand,
                        resolved,
                        session,
                        delivery,
                        held,
                        audit,
                        canonicalOutputDirectory,
                        canonicalQueue,
                        canonicalResults
                    );
                    resultLine = std::move(execution.resultLine);
                    stopAgent  = execution.stopAgent;
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
                >= args.idleTimeout
            )
            {
                UF_TRY(session.close());
                UF_TRY(writer.flush());
                return ok();
            }
            std::this_thread::sleep_for(k_inputAgentPollInterval);
        }
    }
}
