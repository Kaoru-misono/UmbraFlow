#include "queue-ipc.hpp"

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::cli
{
    auto invalid(std::string message) -> std::unexpected<Error>
    {
        return fail(AutomationErrorKind::InvalidResource, std::move(message));
    }

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

    QueueReader::QueueReader(
        std::filesystem::path path,
        QueueNaming naming,
        uintmax startOffset
    )
        : m_path{std::move(path)}
        , m_queueNoun{naming.queue}
        , m_sessionNoun{naming.session}
        , m_offset{startOffset}
    {
    }

    auto QueueReader::readAvailable() -> Result<std::vector<FramedLine>>
    {
        auto error      = std::error_code{};
        auto const size = std::filesystem::file_size(m_path, error);
        if (error)
        {
            return pathFailure("read", m_path, error);
        }
        if (size < m_offset)
        {
            // A queue that shrank was replaced or truncated under the session, so
            // the offset names a different file's bytes and the cursor beside it
            // records lines that are no longer there.
            return invalid(
                std::format(
                    "the {} {} shrank; {} reads one append-only queue",
                    m_queueNoun,
                    m_path.string(),
                    m_sessionNoun
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
                std::format("cannot open the {} {}", m_queueNoun, m_path.string())
            );
        }
        stream.seekg(static_cast<std::streamoff>(m_offset));

        auto appended = std::string{};
        appended.resize(static_cast<std::size_t>(size - m_offset));
        stream.read(appended.data(), static_cast<std::streamsize>(appended.size()));
        if (stream.bad())
        {
            return invalid(
                std::format("cannot read the {} {}", m_queueNoun, m_path.string())
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

    ResultWriter::ResultWriter(std::ofstream stream, std::string label)
        : m_stream{std::move(stream)}
        , m_label{std::move(label)}
    {
    }

    auto ResultWriter::create(
        std::filesystem::path const& path,
        std::string_view label
    ) -> Result<ResultWriter>
    {
        auto stream = std::ofstream{};
        stream.open(path, std::ios::binary | std::ios::app);
        if (!stream.is_open())
        {
            return invalid(
                std::format("cannot open the results file {}", path.string())
            );
        }
        return ResultWriter{std::move(stream), std::string{label}};
    }

    auto ResultWriter::write(std::string_view line) -> Status
    {
        m_stream << line << '\n';
        m_stream.flush();
        if (!m_stream)
        {
            return fail(
                AutomationErrorKind::IoFailure,
                std::format("cannot append to the {} results file", m_label)
            );
        }
        return ok();
    }
}
