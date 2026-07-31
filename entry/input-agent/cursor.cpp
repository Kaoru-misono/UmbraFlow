#include "cursor.hpp"

#include "platform/windows-file-writer.hpp"
#include "platform/windows-path.hpp"

#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace uf::input_agent
{
    namespace
    {
        constexpr auto k_cursorSuffix = std::string_view{".cursor"};
        constexpr auto k_queueScanChunkBytes = std::size_t{64} * 1024U;
        constexpr auto k_maximumCursorBytes = uintmax{64} * 1024U;

        // The fields a cursor file carries. The parser refuses a file missing
        // any of them, which is what makes a half-written cursor detectable
        // rather than believable.
        enum class CursorField : uint8
        {
            Queue,
            ConsumedBytes,
            ConsumedCommands,
        };

        constexpr auto k_cursorFieldKeys = std::array<std::string_view, 3>{
            "queue",
            "consumed-bytes",
            "consumed-commands",
        };

        [[nodiscard]]
        auto cursorFieldKey(CursorField field) noexcept -> std::string_view
        {
            return k_cursorFieldKeys[std::to_underlying(field)];
        }

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
        auto cursorIoFailure(
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

        [[nodiscard]]
        auto cursorFailure(
            std::filesystem::path const& path,
            std::string_view detail
        ) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "input-agent queue cursor {} {}",
                    path.string(),
                    detail
                )
            );
        }

        [[nodiscard]]
        auto parseCursorNumber(
            std::string_view value,
            std::string_view key,
            std::filesystem::path const& path
        ) -> Result<uintmax>
        {
            auto parsed = uintmax{};
            auto const* const begin = std::to_address(value.begin());
            auto const* const end = std::to_address(value.end());
            auto const outcome = std::from_chars(begin, end, parsed);
            if (outcome.ec != std::errc{} || outcome.ptr != end)
            {
                return cursorFailure(
                    path,
                    std::format(
                        "has a non-numeric \"{}\" of \"{}\"",
                        key,
                        value
                    )
                );
            }
            return parsed;
        }

        [[nodiscard]]
        auto writeCursorFile(
            std::filesystem::path const& path,
            InputAgentQueueCursorRecord const& record
        ) -> Status
        {
            auto const text = serializeInputAgentQueueCursor(record);
            UF_TRY_VALUE(
                writer,
                platform::FileWriter::createOrReplace(path)
            );
            UF_TRY(
                writer.write(
                    std::as_bytes(
                        std::span<char const>{
                            text.data(),
                            text.size()
                        }
                    )
                )
            );
            return writer.flushDurably();
        }
    }

    auto inputAgentQueueCursorPath(
        std::filesystem::path const& canonicalQueue
    ) -> std::filesystem::path
    {
        auto path = canonicalQueue;
        path += std::filesystem::path{k_cursorSuffix};
        return path;
    }

    auto serializeInputAgentQueueCursor(
        InputAgentQueueCursorRecord const& record
    ) -> std::string
    {
        return std::format(
            "# umbraflow input-agent queue cursor\n"
            "# The commands in the queue below, up to consumed-bytes, have\n"
            "# already run against the target and been answered in the results\n"
            "# file. A restarting agent resumes after them instead of\n"
            "# delivering them a second time. Delete this file only when you\n"
            "# mean to replay the whole queue.\n"
            "{}={}\n"
            "{}={}\n"
            "{}={}\n",
            cursorFieldKey(CursorField::Queue),
            record.queue.string(),
            cursorFieldKey(CursorField::ConsumedBytes),
            record.position.consumedBytes,
            cursorFieldKey(CursorField::ConsumedCommands),
            record.position.consumedCommands
        );
    }

    auto parseInputAgentQueueCursor(
        std::string_view text,
        std::filesystem::path const& path
    ) -> Result<InputAgentQueueCursorRecord>
    {
        // A cursor is always rewritten whole, so a missing closing newline is
        // the signature of a write that died halfway. Refusing here is what
        // keeps a torn file from parsing as a smaller, plausible position.
        if (!text.ends_with('\n'))
        {
            return cursorFailure(
                path,
                "was cut short: it does not end with a newline"
            );
        }

        auto values = std::array<std::optional<std::string_view>, 3>{};
        auto rest   = text;
        while (!rest.empty())
        {
            auto const newline = rest.find('\n');
            auto line = rest.substr(0, newline);
            rest.remove_prefix(newline + 1U);
            if (line.ends_with('\r'))
            {
                line.remove_suffix(1U);
            }
            if (line.empty() || line.starts_with('#'))
            {
                continue;
            }

            auto const separator = line.find('=');
            if (separator == std::string_view::npos)
            {
                return cursorFailure(
                    path,
                    std::format("has a line without a key: \"{}\"", line)
                );
            }
            auto const key = line.substr(0, separator);
            auto const found = std::ranges::find(k_cursorFieldKeys, key);
            if (found == k_cursorFieldKeys.end())
            {
                return cursorFailure(
                    path,
                    std::format("has an unknown key \"{}\"", key)
                );
            }
            auto const index = static_cast<std::size_t>(
                found - k_cursorFieldKeys.begin()
            );
            if (values[index].has_value())
            {
                return cursorFailure(
                    path,
                    std::format("repeats the key \"{}\"", key)
                );
            }
            values[index] = line.substr(separator + 1U);
        }

        for (auto index = std::size_t{}; index < values.size(); ++index)
        {
            if (!values[index].has_value())
            {
                return cursorFailure(
                    path,
                    std::format("is missing \"{}\"", k_cursorFieldKeys[index])
                );
            }
        }

        UF_TRY_VALUE(
            consumedBytes,
            parseCursorNumber(
                *values[std::to_underlying(CursorField::ConsumedBytes)],
                cursorFieldKey(CursorField::ConsumedBytes),
                path
            )
        );
        UF_TRY_VALUE(
            consumedCommands,
            parseCursorNumber(
                *values[std::to_underlying(CursorField::ConsumedCommands)],
                cursorFieldKey(CursorField::ConsumedCommands),
                path
            )
        );
        return InputAgentQueueCursorRecord{
            .queue = std::filesystem::path{
                *values[std::to_underlying(CursorField::Queue)]
            },
            .position = InputAgentQueuePosition{
                .consumedBytes    = consumedBytes,
                .consumedCommands = consumedCommands,
            },
        };
    }

    auto readInputAgentQueueCursor(
        std::filesystem::path const& path,
        std::filesystem::path const& canonicalQueue
    ) -> Result<std::optional<InputAgentQueuePosition>>
    {
        auto error = std::error_code{};
        auto const status = std::filesystem::symlink_status(path, error);
        if (error == std::errc::no_such_file_or_directory)
        {
            return std::optional<InputAgentQueuePosition>{};
        }
        if (error)
        {
            return cursorIoFailure("inspect queue cursor", path, error);
        }
        if (status.type() == std::filesystem::file_type::not_found)
        {
            return std::optional<InputAgentQueuePosition>{};
        }
        if (status.type() != std::filesystem::file_type::regular)
        {
            return cursorFailure(path, "is not a regular file");
        }

        auto const size = std::filesystem::file_size(path, error);
        if (error)
        {
            return cursorIoFailure("inspect queue cursor", path, error);
        }
        if (size > k_maximumCursorBytes)
        {
            return cursorFailure(
                path,
                std::format(
                    "is larger than the {} bytes a cursor can occupy",
                    k_maximumCursorBytes
                )
            );
        }

        errno       = 0;
        auto stream = std::ifstream{path, std::ios::binary};
        if (!stream.is_open())
        {
            return cursorIoFailure(
                "open queue cursor",
                path,
                currentIoError()
            );
        }
        errno = 0;
        auto const text = std::string{
            std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{}
        };
        if (stream.bad())
        {
            return cursorIoFailure(
                "read queue cursor",
                path,
                currentIoError()
            );
        }

        UF_TRY_VALUE(record, parseInputAgentQueueCursor(text, path));
        if (!platform::pathsEqualOrdinal(record.queue, canonicalQueue))
        {
            return cursorFailure(
                path,
                std::format(
                    "records queue {}, not {}",
                    record.queue.string(),
                    canonicalQueue.string()
                )
            );
        }
        return std::optional<InputAgentQueuePosition>{record.position};
    }

    auto measureInputAgentQueue(
        std::filesystem::path const& queue
    ) -> Result<InputAgentQueueExtent>
    {
        errno       = 0;
        auto stream = std::ifstream{queue, std::ios::binary};
        if (!stream.is_open())
        {
            return cursorIoFailure("open queue file", queue, currentIoError());
        }

        auto extent = InputAgentQueueExtent{};
        auto chunk  = std::string(k_queueScanChunkBytes, '\0');
        while (stream)
        {
            errno = 0;
            stream.read(
                chunk.data(),
                static_cast<std::streamsize>(chunk.size())
            );
            auto const chunkView = std::string_view{
                chunk.data(),
                static_cast<std::size_t>(stream.gcount())
            };
            for (auto const byte : chunkView)
            {
                ++extent.totalBytes;
                if (byte == '\n')
                {
                    extent.framedBytes = extent.totalBytes;
                    ++extent.framedCommands;
                }
            }
        }
        if (stream.bad())
        {
            return cursorIoFailure("read queue file", queue, currentIoError());
        }
        return extent;
    }

    auto resolveInputAgentQueueStart(
        std::optional<InputAgentQueuePosition> const& recorded,
        InputAgentQueueExtent const& extent,
        InputAgentQueueStart start
    ) -> Result<InputAgentQueuePosition>
    {
        if (recorded)
        {
            // A queue is append-only, so a cursor beyond its end means the file
            // was truncated or swapped underneath the agent. Resuming there
            // would either replay or skip, and neither can be chosen blind.
            if (recorded->consumedBytes > extent.totalBytes)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "input-agent queue cursor records {} consumed bytes "
                        "but the queue now holds only {}; it was truncated or "
                        "replaced",
                        recorded->consumedBytes,
                        extent.totalBytes
                    )
                );
            }
            return *recorded;
        }
        if (extent.totalBytes == 0U)
        {
            return InputAgentQueuePosition{};
        }

        switch (start)
        {
            case InputAgentQueueStart::Beginning:
                return InputAgentQueuePosition{};
            case InputAgentQueueStart::End:
                return InputAgentQueuePosition{
                    .consumedBytes    = extent.framedBytes,
                    .consumedCommands = extent.framedCommands,
                };
            case InputAgentQueueStart::Refuse:
                break;
        }
        return fail(
            AutomationErrorKind::InvalidResource,
            std::format(
                "input-agent found no queue cursor beside a queue that already "
                "holds {} commands in {} bytes; starting at its beginning would "
                "deliver every one of them to the target again. Pass "
                "--queue-start beginning to run them, or --queue-start end to "
                "treat them as already delivered",
                extent.framedCommands,
                extent.totalBytes
            )
        );
    }

    InputAgentQueueCursor::InputAgentQueueCursor(
        std::filesystem::path path,
        std::filesystem::path queue,
        InputAgentQueuePosition position
    ) noexcept
        : m_path{std::move(path)}
        , m_queue{std::move(queue)}
        , m_position{position}
    {
    }

    auto InputAgentQueueCursor::open(
        std::filesystem::path path,
        std::filesystem::path canonicalQueue,
        InputAgentQueuePosition start
    ) -> Result<InputAgentQueueCursor>
    {
        UF_TRY(
            writeCursorFile(
                path,
                InputAgentQueueCursorRecord{
                    .queue    = canonicalQueue,
                    .position = start,
                }
            )
        );
        return InputAgentQueueCursor{
            std::move(path),
            std::move(canonicalQueue),
            start
        };
    }

    auto InputAgentQueueCursor::position() const noexcept
        -> InputAgentQueuePosition
    {
        return m_position;
    }

    auto InputAgentQueueCursor::advance(uintmax consumedBytes) -> Status
    {
        auto const advanced = InputAgentQueuePosition{
            .consumedBytes    = consumedBytes,
            .consumedCommands = m_position.consumedCommands + 1U,
        };
        UF_TRY(
            writeCursorFile(
                m_path,
                InputAgentQueueCursorRecord{
                    .queue    = m_queue,
                    .position = advanced,
                }
            )
        );
        // Only a durable write moves the in-memory position, so the two never
        // disagree about what a restart may skip.
        m_position = advanced;
        return ok();
    }
}
