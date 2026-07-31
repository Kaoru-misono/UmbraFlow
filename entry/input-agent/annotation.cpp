#include "annotation.hpp"

#include "capture-output.hpp"
#include "drive.hpp"
#include "error-text.hpp"
#include "json-string.hpp"
#include "loop.hpp"
#include "path-validation.hpp"
#include "platform/windows-file-writer.hpp"
#include "protocol.hpp"

#include <controller/discovery.hpp>
#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>
#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/space.hpp>

#include <filesystem>
#include <format>
#include <memory>
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
            IInputAgentDrive& drive,
            OutputFile& output
        ) -> Result<Frame>
        {
            UF_TRY_VALUE(frame, drive.capture());
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

        [[nodiscard]]
        auto finishAction(
            std::string_view operation,
            InputAgentActionResult const& result,
            bool stopAgent = false
        ) -> InputAgentCommandOutcome
        {
            return InputAgentCommandOutcome{
                .resultLine = serializeInputAgentActionResult(operation, result),
                .stopAgent  = stopAgent,
            };
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
            // only after delivery; see runFramedAction.
            Frame frame;
        };

        // Reserves both output files before observing, so a path that is not
        // confined, aliases another output, or already exists is refused before
        // the target is looked at at all. The before-frame is deliberately left
        // unencoded here: the slow encode plus durable flush must not sit inside
        // the observe->act window, where it would inflate this observation's age
        // against max_action_frame_age and produce a false StaleObservation.
        [[nodiscard]]
        auto beginFramedAction(
            InputAgentFramePaths const& paths,
            std::filesystem::path const& canonicalOutputDirectory,
            IInputAgentDrive& drive
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
            UF_TRY_VALUE(frame, drive.capture());
            return FramedAction{
                .before = std::move(before),
                .after  = std::move(after),
                .frame  = std::move(frame),
            };
        }

        // Everything a framed action does around one delivery, which is the same
        // for all three inputs: reserve the outputs, observe, deliver from that
        // observation, then write the two frames that bracket it.
        //
        // `deliver` is called exactly once and never stored, so it may capture by
        // reference; it is a template rather than a type-erased callable because
        // its instantiation set is the three run overloads below and nothing
        // else.
        template <typename Deliver>
        [[nodiscard]]
        auto runFramedAction(
            std::string_view operation,
            InputAgentFramePaths const& paths,
            MonotonicInstant::Duration settle,
            std::filesystem::path const& canonicalOutputDirectory,
            IInputAgentDrive& drive,
            Deliver deliver
        ) -> InputAgentCommandOutcome
        {
            auto result      = InputAgentActionResult{};
            auto settleBound = checkSettleBound(operation, settle);
            if (!settleBound)
            {
                result.error = std::move(settleBound).error();
                return finishAction(operation, result);
            }

            auto action = beginFramedAction(
                paths,
                canonicalOutputDirectory,
                drive
            );
            if (!action)
            {
                result.error = std::move(action).error();
                return finishAction(operation, result);
            }
            result.beforeFrame = action->frame.id().value();

            auto outcome = deliver(action->frame);
            result.delivered = outcome.delivered();
            if (!outcome.delivered())
            {
                result.error = std::move(outcome.error);
                return finishAction(operation, result, outcome.targetReplaced);
            }

            // The before-Frame is immutable, so its file still shows the
            // pre-action observation even though it is encoded now. Both output
            // handles were reserved with CREATE_NEW before the capture, so
            // moving the encode past delivery does not weaken path confinement.
            auto written = writeFramePng(action->frame, action->before);
            if (!written)
            {
                result.error = std::move(written).error();
                return finishAction(operation, result);
            }
            if (settle > MonotonicInstant::Duration::zero())
            {
                std::this_thread::sleep_for(settle);
            }

            auto after = captureToOutput(drive, action->after);
            if (!after)
            {
                result.error = std::move(after).error();
                return finishAction(operation, result);
            }
            result.afterFrame = after->id().value();
            return finishAction(operation, result);
        }
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

    AnnotationSession::AnnotationSession(
        std::unique_ptr<IInputAgentDrive> p_drive,
        std::filesystem::path canonicalOutputDirectory,
        std::filesystem::path canonicalQueue,
        std::filesystem::path canonicalResults
    )
        : m_drive{std::move(p_drive)}
        , m_outputDirectory{std::move(canonicalOutputDirectory)}
        , m_queue{std::move(canonicalQueue)}
        , m_results{std::move(canonicalResults)}
    {
    }

    auto AnnotationSession::run(
        InputAgentCaptureCommand const& command
    ) -> InputAgentCommandOutcome
    {
        auto const validated = validateOutputPath(
            command.output,
            m_outputDirectory,
            m_queue,
            m_results,
            "capture output"
        );
        if (!validated)
        {
            return InputAgentCommandOutcome{
                .resultLine = serializeCaptureError(validated.error()),
                .stopAgent  = false,
            };
        }

        auto output = openOutputFile(*validated, m_outputDirectory);
        if (!output)
        {
            return InputAgentCommandOutcome{
                .resultLine = serializeCaptureError(output.error()),
                .stopAgent  = false,
            };
        }

        auto captured = captureToOutput(*m_drive, *output);
        if (!captured)
        {
            return InputAgentCommandOutcome{
                .resultLine = serializeCaptureError(captured.error()),
                .stopAgent  = false,
            };
        }
        return InputAgentCommandOutcome{
            .resultLine = serializeCaptureSuccess(
                *captured,
                m_drive->clientSize()
            ),
            .stopAgent = false,
        };
    }

    auto AnnotationSession::run(
        InputAgentClickCommand const& command
    ) -> InputAgentCommandOutcome
    {
        auto constexpr operation = std::string_view{"click"};
        auto paths = resolveInputAgentFramePaths(
            command.outputBefore,
            command.outputAfter,
            m_outputDirectory,
            m_queue,
            m_results,
            operation
        );
        if (!paths)
        {
            auto result  = InputAgentActionResult{};
            result.error = std::move(paths).error();
            return finishAction(operation, result);
        }

        auto const point = Point<ClientSpace>{command.x, command.y};
        return runFramedAction(
            operation,
            *paths,
            command.settle,
            m_outputDirectory,
            *m_drive,
            [this, point](Frame const& observation) -> DriveOutcome
            {
                return m_drive->click(observation, point);
            }
        );
    }

    auto AnnotationSession::run(
        InputAgentKeyCommand const& command
    ) -> InputAgentCommandOutcome
    {
        auto constexpr operation = std::string_view{"key"};
        auto paths = resolveInputAgentFramePaths(
            command.outputBefore,
            command.outputAfter,
            m_outputDirectory,
            m_queue,
            m_results,
            operation
        );
        if (!paths)
        {
            auto result  = InputAgentActionResult{};
            result.error = std::move(paths).error();
            return finishAction(operation, result);
        }

        return runFramedAction(
            operation,
            *paths,
            command.settle,
            m_outputDirectory,
            *m_drive,
            [this, key = command.key](Frame const& observation) -> DriveOutcome
            {
                return m_drive->key(observation, key);
            }
        );
    }

    auto AnnotationSession::run(
        InputAgentScrollCommand const& command
    ) -> InputAgentCommandOutcome
    {
        // The after-frame matters more here than anywhere else: a scrolled list
        // that had already reached its end looks identical to one that never
        // received the wheel at all, and only the two frames tell them apart.
        auto constexpr operation = std::string_view{"scroll"};
        auto paths = resolveInputAgentFramePaths(
            command.outputBefore,
            command.outputAfter,
            m_outputDirectory,
            m_queue,
            m_results,
            operation
        );
        if (!paths)
        {
            auto result  = InputAgentActionResult{};
            result.error = std::move(paths).error();
            return finishAction(operation, result);
        }

        auto const point = Point<ClientSpace>{command.x, command.y};
        return runFramedAction(
            operation,
            *paths,
            command.settle,
            m_outputDirectory,
            *m_drive,
            [this, point, delta = command.delta](
                Frame const& observation
            ) -> DriveOutcome
            {
                return m_drive->scroll(observation, point, delta);
            }
        );
    }

    auto AnnotationSession::run(
        InputAgentQuitCommand const&
    ) -> InputAgentCommandOutcome
    {
        UF_UNREACHABLE_MSG(
            "input-agent quit never reaches the session: the loop answers it"
        );
    }

    auto AnnotationSession::execute(
        InputAgentCommand const& command
    ) -> InputAgentCommandOutcome
    {
        return std::visit(
            [this](auto const& specific) -> InputAgentCommandOutcome
            {
                return run(specific);
            },
            command
        );
    }

    auto AnnotationSession::clearCommandAudit() noexcept -> void
    {
        m_drive->clearAudit();
    }

    auto AnnotationSession::close() -> Status
    {
        return m_drive->close();
    }
}
