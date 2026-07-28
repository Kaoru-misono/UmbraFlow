#include "file-sink.hpp"

#include "event.hpp"

#include <core/error/contracts.hpp>

#include <domain/error.hpp>

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
        [[nodiscard]]
        auto currentIoError() -> std::error_code
        {
            if (errno != 0)
            {
                return std::error_code{errno, std::generic_category()};
            }
            return std::make_error_code(std::io_errc::stream);
        }
    }

    FileTraceSink::FileTraceSink(OpenTag, std::ofstream stream) noexcept
        : m_stream{std::move(stream)}
    {
        UF_CHECK(m_stream.is_open());
    }

    auto FileTraceSink::create(
        std::filesystem::path const& path
    ) -> Result<std::unique_ptr<ITraceSink>>
    {
        auto stream = std::ofstream{};
        errno       = 0;
        stream.open(path, std::ios::binary | std::ios::trunc);
        if (!stream.is_open())
        {
            return fail(
                AutomationErrorKind::IoFailure,
                "cannot open trace file "
                    + path.string()
                    + ": "
                    + currentIoError().message()
            );
        }

        auto p_sink = std::make_unique<FileTraceSink>(OpenTag{}, std::move(stream));
        return std::unique_ptr<ITraceSink>{std::move(p_sink)};
    }

    auto FileTraceSink::emit(StampedTraceEvent const& event) -> Status
    {
        errno = 0;
        m_stream << serializeTraceEvent(event) << '\n';
        m_stream.flush();
        if (!m_stream)
        {
            return fail(
                AutomationErrorKind::IoFailure,
                "trace emit failed: " + currentIoError().message()
            );
        }
        return ok();
    }
}
