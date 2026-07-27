#include <task/file-trace-sink.hpp>

#include <task/trace.hpp>

#include <core/error/contracts.hpp>

#include <domain/error.hpp>

#include <cerrno>
#include <ios>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace uf::task
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

    FileTaskTraceSink::FileTaskTraceSink(OpenTag, std::ofstream stream) noexcept
        : m_stream{std::move(stream)}
    {
        UF_CHECK(m_stream.is_open());
    }

    auto FileTaskTraceSink::create(
        std::filesystem::path const& path
    ) -> Result<std::unique_ptr<TaskTraceSink>>
    {
        auto stream = std::ofstream{};
        errno       = 0;
        stream.open(path, std::ios::binary | std::ios::trunc);
        if (!stream.is_open())
        {
            return fail(
                AutomationErrorKind::IoFailure,
                "cannot open task trace file "
                    + path.string()
                    + ": "
                    + currentIoError().message()
            );
        }

        auto p_sink = std::make_unique<FileTaskTraceSink>(OpenTag{}, std::move(stream));
        return std::unique_ptr<TaskTraceSink>{std::move(p_sink)};
    }

    auto FileTaskTraceSink::emit(TaskTraceEvent const& event) -> Status
    {
        errno = 0;
        m_stream << serializeTaskTraceEvent(event) << '\n';
        m_stream.flush();
        if (!m_stream)
        {
            return fail(
                AutomationErrorKind::IoFailure,
                "task trace emit failed: " + currentIoError().message()
            );
        }
        return ok();
    }
}
