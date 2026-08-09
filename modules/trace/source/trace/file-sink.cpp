#include "file-sink.hpp"

#include "event.hpp"

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>

#include <cerrno>
#include <ios>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace uf::trace
{
    namespace
    {
        [[nodiscard]] auto currentIoError() -> std::error_code
        {
            if (errno != 0)
            {
                return std::error_code{errno, std::generic_category()};
            }
            return std::make_error_code(std::io_errc::stream);
        }

        [[nodiscard]]
        auto ioFailure(
            std::string message,
            std::error_code nativeCode = {}
        ) -> std::unexpected<Error>
        {
            return fail(
                std::make_error_code(std::errc::io_error),
                std::move(message),
                nativeCode
            );
        }
    }

    FileTraceSink::FileTraceSink(OpenTag, std::ofstream stream) noexcept
        : m_stream{std::move(stream)}
    {
        UF_CHECK(m_stream.is_open());
    }

    auto FileTraceSink::createNew(
        std::filesystem::path const& path
    ) -> Result<std::unique_ptr<ITraceSink>>
    {
        auto stream = std::ofstream{};
        errno       = 0;
        stream.open(path, std::ios::binary | std::ios::out | std::ios::app);
        if (!stream.is_open())
        {
            return ioFailure(
                "cannot open new trace file " + path.string(),
                currentIoError()
            );
        }

        errno          = 0;
        stream.seekp(0, std::ios::end);
        auto const end = stream.tellp();
        if (!stream || end == std::ofstream::pos_type{-1})
        {
            return ioFailure(
                "cannot inspect trace file " + path.string(),
                currentIoError()
            );
        }
        if (end != std::ofstream::pos_type{0})
        {
            return fail(
                std::make_error_code(std::errc::file_exists),
                "trace file already contains evidence: " + path.string()
            );
        }

        auto p_sink = std::make_unique<FileTraceSink>(
            OpenTag{},
            std::move(stream)
        );
        return std::unique_ptr<ITraceSink>{std::move(p_sink)};
    }

    auto FileTraceSink::append(TraceEvent const& event) -> Status
    {
        auto const line = serializeTraceEvent(event);
        errno           = 0;
        m_stream << line << '\n';
        m_stream.flush();
        if (!m_stream)
        {
            return ioFailure("trace append failed", currentIoError());
        }
        return ok();
    }
}
