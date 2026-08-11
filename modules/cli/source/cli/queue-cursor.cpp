#include "queue-cursor.hpp"

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <charconv>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::cli
{
    namespace
    {
        constexpr auto k_cursorSuffix        = std::string_view{".cursor"};
        constexpr auto k_maximumCursorBytes  = uintmax{64} * 1024U;
        constexpr auto k_queueScanChunkBytes = std::size_t{64} * 1024U;

        // `consumed-chunks` is the on-disk spelling of `consumedLines`. Cursors an
        // operator already holds carry that key, and an unrecognized key is a
        // refusal rather than a default.
        constexpr auto k_consumedLinesKey = std::string_view{"consumed-chunks"};

        [[nodiscard]]
        auto cursorIoFailure(
            std::string_view operation,
            std::filesystem::path const& path,
            std::error_code error
        ) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::IoFailure,
                std::format(
                    "cannot {} {}: {}",
                    operation,
                    path.string(),
                    error.message()
                ),
                error
            );
        }

        [[nodiscard]]
        auto cursorFailure(
            std::filesystem::path const& path,
            std::string_view detail
        ) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format("the queue cursor {} {}", path.string(), detail)
            );
        }

        [[nodiscard]]
        auto parseCursorNumber(
            std::string_view value,
            std::string_view key,
            std::filesystem::path const& path
        ) -> Result<uintmax>
        {
            auto              parsed = uintmax{};
            auto const* const begin  = std::to_address(value.begin());
            auto const* const end    = std::to_address(value.end());
            auto const        outcome = std::from_chars(begin, end, parsed);
            if (outcome.ec != std::errc{} || outcome.ptr != end)
            {
                return cursorFailure(
                    path,
                    std::format("has a non-numeric \"{}\" of \"{}\"", key, value)
                );
            }
            return parsed;
        }
    }

    auto queueCursorPath(
        std::filesystem::path const& canonicalQueue
    ) -> std::filesystem::path
    {
        auto path = canonicalQueue;
        path += std::filesystem::path{k_cursorSuffix};
        return path;
    }

    auto serializeQueueCursor(QueueCursorRecord const& record) -> std::string
    {
        // Generic-form so a cursor written on one host reads identically on
        // another, and so a Windows path in it never carries a backslash a reader
        // has to un-escape.
        return std::format(
            "queue={}\nconsumed-bytes={}\n{}={}\n",
            record.queue.generic_string(),
            record.position.consumedBytes,
            k_consumedLinesKey,
            record.position.consumedLines
        );
    }

    auto parseQueueCursor(
        std::string_view text,
        std::filesystem::path const& path
    ) -> Result<QueueCursorRecord>
    {
        auto queue         = std::optional<std::string>{};
        auto consumedBytes = std::optional<uintmax>{};
        auto consumedLines = std::optional<uintmax>{};

        auto position = std::size_t{0};
        while (position < text.size())
        {
            auto const newline = text.find('\n', position);
            if (newline == std::string_view::npos)
            {
                // No terminator: the file was cut short mid-write, so nothing
                // after the last complete line can be believed -- and neither can
                // the record, because a missing field is not a default.
                return cursorFailure(path, "ends without a newline");
            }
            auto line = text.substr(position, newline - position);
            position  = newline + 1U;
            if (!line.empty() && line.back() == '\r')
            {
                line.remove_suffix(1U);
            }
            if (line.empty())
            {
                continue;
            }

            auto const separator = line.find('=');
            if (separator == std::string_view::npos)
            {
                return cursorFailure(
                    path,
                    std::format("has a line with no '=': \"{}\"", line)
                );
            }
            auto const key   = line.substr(0, separator);
            auto const value = line.substr(separator + 1U);

            if (key == "queue")
            {
                queue = std::string{value};
            }
            else if (key == "consumed-bytes")
            {
                UF_TRY_VALUE(parsed, parseCursorNumber(value, key, path));
                consumedBytes = parsed;
            }
            else if (key == k_consumedLinesKey)
            {
                UF_TRY_VALUE(parsed, parseCursorNumber(value, key, path));
                consumedLines = parsed;
            }
            else
            {
                return cursorFailure(
                    path,
                    std::format("has an unrecognized field \"{}\"", key)
                );
            }
        }

        if (
            !queue.has_value()
            || !consumedBytes.has_value()
            || !consumedLines.has_value()
        )
        {
            return cursorFailure(path, "is missing a field");
        }

        return QueueCursorRecord{
            .queue    = std::filesystem::path{*queue},
            .position = QueuePosition{
                .consumedBytes = *consumedBytes,
                .consumedLines = *consumedLines,
            },
        };
    }

    auto readQueueCursor(
        std::filesystem::path const& path,
        std::filesystem::path const& canonicalQueue
    ) -> Result<std::optional<QueuePosition>>
    {
        auto error       = std::error_code{};
        auto const state = std::filesystem::status(path, error);
        if (error && error != std::errc::no_such_file_or_directory)
        {
            return cursorIoFailure("inspect", path, error);
        }
        if (!std::filesystem::exists(state))
        {
            return std::optional<QueuePosition>{};
        }
        if (!std::filesystem::is_regular_file(state))
        {
            return cursorFailure(path, "is not a regular file");
        }

        error           = std::error_code{};
        auto const size = std::filesystem::file_size(path, error);
        if (error)
        {
            return cursorIoFailure("measure", path, error);
        }
        if (size > k_maximumCursorBytes)
        {
            return cursorFailure(path, "is larger than a cursor can be");
        }

        auto stream = std::ifstream{path, std::ios::binary};
        if (!stream.is_open())
        {
            return cursorIoFailure(
                "open",
                path,
                std::make_error_code(std::io_errc::stream)
            );
        }
        auto text = std::string(static_cast<std::size_t>(size), '\0');
        stream.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (stream.bad())
        {
            return cursorIoFailure(
                "read",
                path,
                std::make_error_code(std::io_errc::stream)
            );
        }
        text.resize(static_cast<std::size_t>(stream.gcount()));

        UF_TRY_VALUE(record, parseQueueCursor(text, path));
        if (record.queue.generic_string() != canonicalQueue.generic_string())
        {
            return cursorFailure(
                path,
                std::format(
                    "describes the queue {}, not {}",
                    record.queue.generic_string(),
                    canonicalQueue.generic_string()
                )
            );
        }
        return std::optional<QueuePosition>{record.position};
    }

    auto measureQueueExtent(
        std::filesystem::path const& queue
    ) -> Result<QueueExtent>
    {
        auto error      = std::error_code{};
        auto const size = std::filesystem::file_size(queue, error);
        if (error)
        {
            return cursorIoFailure("measure", queue, error);
        }

        auto stream = std::ifstream{queue, std::ios::binary};
        if (!stream.is_open())
        {
            return cursorIoFailure(
                "open",
                queue,
                std::make_error_code(std::io_errc::stream)
            );
        }

        auto extent  = QueueExtent{.totalBytes = size};
        auto buffer  = std::vector<char>(k_queueScanChunkBytes, '\0');
        auto scanned = uintmax{0};
        while (stream)
        {
            stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            auto const read = static_cast<std::size_t>(stream.gcount());
            for (auto index = std::size_t{0}; index < read; ++index)
            {
                ++scanned;
                if (buffer[index] == '\n')
                {
                    extent.framedBytes = scanned;
                    ++extent.framedLines;
                }
            }
            if (read == 0U)
            {
                break;
            }
        }
        if (stream.bad())
        {
            return cursorIoFailure(
                "read",
                queue,
                std::make_error_code(std::io_errc::stream)
            );
        }
        return extent;
    }

    auto resolveQueueStart(
        std::optional<QueuePosition> const& recorded,
        QueueExtent const& extent,
        std::filesystem::path const& queue
    ) -> Result<QueuePosition>
    {
        if (recorded.has_value())
        {
            if (recorded->consumedBytes > extent.totalBytes)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "the cursor beside {} has consumed {} bytes of a file "
                        "that holds only {}; the queue was truncated or replaced "
                        "under the session",
                        queue.string(),
                        recorded->consumedBytes,
                        extent.totalBytes
                    )
                );
            }
            return *recorded;
        }

        if (extent.totalBytes == 0U)
        {
            return QueuePosition{};
        }

        return fail(
            AutomationErrorKind::InvalidResource,
            std::format(
                "the queue {} already holds {} line(s) and no cursor records what "
                "ran; running them could re-deliver clicks and keystrokes against "
                "a live target, and skipping them could drop work. Start from an "
                "empty queue, or restore the cursor this queue was written for",
                queue.string(),
                extent.framedLines
            )
        );
    }

    QueueCursor::QueueCursor(
        std::filesystem::path path,
        std::filesystem::path queue,
        QueuePosition position
    ) noexcept
        : m_path{std::move(path)}
        , m_queue{std::move(queue)}
        , m_position{position}
    {
    }

    auto QueueCursor::open(
        std::filesystem::path path,
        std::filesystem::path canonicalQueue,
        QueuePosition start
    ) -> Result<QueueCursor>
    {
        auto cursor = QueueCursor{
            std::move(path),
            std::move(canonicalQueue),
            start,
        };
        UF_TRY(cursor.advance(start.consumedBytes));
        return cursor;
    }

    auto QueueCursor::advance(uintmax consumedBytes) -> Status
    {
        if (consumedBytes > m_position.consumedBytes)
        {
            m_position.consumedBytes = consumedBytes;
            ++m_position.consumedLines;
        }

        auto stream = std::ofstream{m_path, std::ios::binary | std::ios::trunc};
        if (!stream.is_open())
        {
            return fail(
                AutomationErrorKind::IoFailure,
                std::format("cannot write the queue cursor {}", m_path.string())
            );
        }
        stream << serializeQueueCursor(
            QueueCursorRecord{
                .queue    = m_queue,
                .position = m_position,
            }
        );
        stream.flush();
        if (!stream)
        {
            return fail(
                AutomationErrorKind::IoFailure,
                std::format("cannot write the queue cursor {}", m_path.string())
            );
        }
        return ok();
    }
}
