#include "agent.hpp"

#include "annotation.hpp"
#include "args.hpp"
#include "cursor.hpp"
#include "drive.hpp"
#include "loop.hpp"
#include "ocr-text-reader.hpp"
#include "path-validation.hpp"
#include "protocol.hpp"
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

#include <trace/event.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::input_agent
{
    namespace
    {
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
        std::filesystem::path path,
        uintmax startBytes
    ) noexcept
        : m_path{std::move(path)}
        , m_offset{startBytes}
    {
    }

    auto InputAgentQueueReader::extractEntries()
        -> Result<std::vector<Entry>>
    {
        // Every byte in m_pending was read from the file and bytes only leave
        // it from the front, so this is the queue offset m_pending[0] sits at.
        auto const pendingSize = static_cast<uintmax>(m_pending.size());
        UF_ASSERT(m_offset >= pendingSize);
        auto const pendingBase = m_offset - pendingSize;

        auto entries  = std::vector<Entry>{};
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
            consumed              = newline + 1U;
            auto const entryBytes = pendingBase + static_cast<uintmax>(consumed);
            entries.emplace_back(
                Entry{
                    .text          = std::move(line),
                    .consumedBytes = entryBytes,
                }
            );
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
        return entries;
    }

    auto InputAgentQueueReader::create(
        std::filesystem::path path,
        uintmax startBytes
    ) -> Result<InputAgentQueueReader>
    {
        errno       = 0;
        auto stream = std::ifstream{path, std::ios::binary};
        if (!stream.is_open())
        {
            return agentIoFailure("open queue file", path, currentIoError());
        }
        return InputAgentQueueReader{std::move(path), startBytes};
    }

    auto InputAgentQueueReader::readAvailable()
        -> Result<std::vector<Entry>>
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
            return extractEntries();
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
        return extractEntries();
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

        // The cursor is what stops a restarted agent from walking the target
        // through every command the queue already holds. It is derived from the
        // queue path so the pairing is legible on disk, and it must not land on
        // either IPC file, which a hard link could otherwise arrange.
        auto const cursorPath = inputAgentQueueCursorPath(canonicalQueue);
        UF_TRY_VALUE(
            cursorAliasesQueue,
            canonicalPathsAlias(cursorPath, canonicalQueue)
        );
        UF_TRY_VALUE(
            cursorAliasesResults,
            canonicalPathsAlias(cursorPath, canonicalResults)
        );
        if (cursorAliasesQueue || cursorAliasesResults)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "input-agent queue cursor {} must not be the queue or the "
                    "results file",
                    cursorPath.string()
                )
            );
        }

        UF_TRY_VALUE(
            recordedPosition,
            readInputAgentQueueCursor(cursorPath, canonicalQueue)
        );
        UF_TRY_VALUE(queueExtent, measureInputAgentQueue(canonicalQueue));
        UF_TRY_VALUE(
            startPosition,
            resolveInputAgentQueueStart(
                recordedPosition,
                queueExtent,
                args.queueStart
            )
        );
        UF_TRY_VALUE(
            cursor,
            InputAgentQueueCursor::open(
                cursorPath,
                canonicalQueue,
                startPosition
            )
        );
        UF_TRY_VALUE(
            reader,
            InputAgentQueueReader::create(
                canonicalQueue,
                startPosition.consumedBytes
            )
        );
        // The one place this run says who it is. An annotation session is the
        // third front-end -- neither the task the trusted Luau framework runs
        // nor the operator driving a loaded project -- and stating it here
        // rather than inside the writer is what keeps the writer a writer.
        UF_TRY_VALUE(
            writer,
            InputAgentResultWriter::create(
                canonicalResults,
                trace::FrontEnd::Annotation
            )
        );
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
            captureSession,
            createCaptureSession(
                resolved,
                sessionId,
                WgcCaptureOptions{}
            )
        );

        // The reader is built here and never loads anything: the model comes up
        // on the first `read` command, so a run that only captures pays nothing
        // for it and a run whose payload is missing still serves every other
        // verb. What can fail here is naming this executable's own directory,
        // which is where the payload is looked for.
        UF_TRY_VALUE(textReader, createOcrTextReader());

        auto session = AnnotationSession{
            std::make_unique<WindowInputAgentDrive>(
                std::move(resolved),
                std::move(captureSession),
                delivery,
                client
            ),
            std::move(textReader),
            canonicalOutputDirectory,
            canonicalQueue,
            canonicalResults,
        };
        auto clock = SystemInputAgentPollClock{};
        return runInputAgentQueueLoop(
            reader,
            cursor,
            writer,
            session,
            clock,
            args.idleTimeout
        );
    }
}
